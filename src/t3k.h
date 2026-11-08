#pragma once
// TONE3000 API client (device-code sign-in, search, model list, download).
// Endpoints and field names follow the official plugin's Tone3000Client (v0.0.12).
// All calls here are BLOCKING: run them on a worker thread (see Task in main.cpp).
// HTTP is behind IHttp so the logic is testable without a network.
#include <atomic>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "json.h"

struct HttpResp {
    long status = 0;      // 0 = transport failure (see error)
    std::string body;
    std::string error;    // transport-level error text
    bool ok() const { return status >= 200 && status < 300; }
};

class IHttp {
public:
    virtual ~IHttp() = default;
    // headers: full "Name: value" lines.
    virtual HttpResp request(const std::string& method, const std::string& url,
                             const std::vector<std::string>& headers, const std::string& body, int timeoutSec) = 0;
    // Streams the response body to destPath. `bytes` receives progress, `cancel` aborts.
    virtual HttpResp download(const std::string& url, const std::vector<std::string>& headers,
                              const std::string& destPath, std::atomic<long long>* bytes,
                              std::atomic<bool>* cancel, int timeoutSec) = 0;
};

inline std::string urlEscape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

inline std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string o;
    for (auto& p : kv) {
        if (!o.empty()) o += '&';
        o += urlEscape(p.first) + "=" + urlEscape(p.second);
    }
    return o;
}

// Safe file/folder name for a FAT32 SD card.
inline std::string sanitizeFileName(const std::string& in, size_t maxLen = 80) {
    std::string o;
    for (unsigned char c : in) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            o += '_';
        else
            o += (char)c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    size_t st = 0;
    while (st < o.size() && (o[st] == ' ' || o[st] == '.')) ++st;
    o = o.substr(st);
    if (o.size() > maxLen) {
        o.resize(maxLen);
        // do not cut a UTF-8 sequence in half
        while (!o.empty() && ((unsigned char)o.back() & 0xC0) == 0x80) o.pop_back();
        if (!o.empty() && (unsigned char)o.back() >= 0xC0) o.pop_back();
    }
    return o.empty() ? "unnamed" : o;
}

struct Tone {
    int id = 0;
    std::string title, user, gear, format;
    int models = 0, a2 = 0, downloads = 0;
};
struct ModelInfo {
    int id = 0;
    std::string name, url, size;
};
struct DeviceAuth {
    std::string deviceCode, userCode, verifyUri, verifyUriComplete;
    int expiresIn = 900, interval = 5;
};
struct ToneList {
    bool ok = false;
    std::string err;
    std::vector<Tone> tones;
    int page = 1, totalPages = 1;
};
struct ModelList {
    bool ok = false;
    std::string err;
    std::vector<ModelInfo> models;
};

class T3kClient {
public:
    explicit T3kClient(IHttp& h) : http_(h) {}

    std::string origin = "https://www.tone3000.com";
    std::string key;        // publishable key "t3k_pub_..." = OAuth client_id
    std::string tokenFile;  // where the session is kept between runs
    std::function<int64_t()> now = [] { return (int64_t)time(nullptr); };

    bool hasKey() const { return !key.empty(); }

    bool loadTokens() {
        std::ifstream f(tokenFile);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        bool ok = false;
        Json j = Json::parse(ss.str(), &ok);
        if (!ok || j["refresh"].str().empty()) return false;
        std::lock_guard<std::mutex> g(mu_);
        access_ = j["access"].str();
        refresh_ = j["refresh"].str();
        expiresAt_ = (int64_t)j["expires_at"].num();
        return true;
    }
    bool signedIn() const {
        std::lock_guard<std::mutex> g(mu_);
        return !refresh_.empty();
    }
    void signOut() {
        {
            std::lock_guard<std::mutex> g(mu_);
            access_.clear();
            refresh_.clear();
            expiresAt_ = 0;
        }
        if (!tokenFile.empty()) remove(tokenFile.c_str());
    }

    // ---- sign-in (RFC 8628 device authorization grant)
    bool startDeviceAuth(DeviceAuth& out, std::string& err) {
        long st = 0;
        std::string body = postForm("/api/v1/oauth/device_authorization", {{"client_id", key}}, st, err);
        if (st == 0) return false;
        Json j = Json::parse(body);
        if (st < 200 || st >= 300) {
            err = j["error"].str().empty() ? "device_authorization_failed" : j["error"].str();
            return false;
        }
        out.deviceCode = j["device_code"].str();
        out.userCode = j["user_code"].str();
        out.verifyUri = j["verification_uri"].str();
        out.verifyUriComplete = j["verification_uri_complete"].str();
        if (out.verifyUriComplete.empty()) out.verifyUriComplete = out.verifyUri;
        out.expiresIn = j["expires_in"].integer(900);
        out.interval = std::max(1, j["interval"].integer(5));
        if (out.deviceCode.empty() || out.userCode.empty() || out.verifyUri.empty()) {
            err = "device_authorization_failed";
            return false;
        }
        return true;
    }

    // "ok" when signed in; "authorization_pending" / "slow_down" mean ask again;
    // anything else (expired_token, access_denied, ...) is final.
    std::string pollDevice(const std::string& deviceCode) {
        long st = 0;
        std::string err;
        std::string body = postForm("/api/v1/oauth/token",
                                    {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                                     {"device_code", deviceCode},
                                     {"client_id", key}},
                                    st, err);
        if (st == 0) return "authorization_pending";  // transport hiccup: keep polling
        Json j = Json::parse(body);
        if (!j["access_token"].str().empty() && !j["refresh_token"].str().empty()) {
            setTokens(j["access_token"].str(), j["refresh_token"].str(), (int64_t)j["expires_in"].num(3600));
            return "ok";
        }
        std::string e = j["error"].str();
        return e.empty() ? "token_failed" : e;
    }

    // ---- catalogue
    // kind: "trending" | "search" | "favorited". ir=true lists impulse responses.
    ToneList listTones(const std::string& kind, const std::string& query, bool ir, int page, int pageSize) {
        ToneList r;
        std::string path;
        if (kind == "trending") {
            path = "/api/v1/tones/trending";
        } else {
            path = "/api/v1/tones/" + kind + "?page=" + std::to_string(page) + "&page_size=" + std::to_string(pageSize);
            if (!query.empty()) path += "&query=" + urlEscape(query);
            if (kind == "search") path += std::string("&format=") + (ir ? "ir" : "nam") + "&architecture=2";
        }
        HttpResp h = authed("GET", path, "", r.err);
        if (h.status == 0 && r.err.empty()) r.err = h.error;
        if (!h.ok()) {
            if (r.err.empty()) r.err = errorText(h);
            return r;
        }
        bool ok = false;
        Json j = Json::parse(h.body, &ok);
        if (!ok) {
            r.err = "bad_response";
            return r;
        }
        const Json& data = j["data"];
        for (const Json& t : data.items()) {
            Tone o;
            o.id = t["id"].integer();
            o.title = t["title"].str();
            o.gear = t["gear"].str();
            o.format = t["format"].str();
            o.models = t["models_count"].integer();
            o.a2 = t["a2_models_count"].integer();
            o.downloads = t["downloads_count"].integer();
            o.user = t["user"]["display_name"].str();
            if (o.user.empty()) o.user = t["user"]["username"].str();
            if (o.id == 0 || o.title.empty()) continue;
            // Impulse-response lists keep IR tones, NAM lists keep tones that have A2 models.
            if (!o.format.empty() && o.format != (ir ? "ir" : "nam")) continue;  // favourites mix both formats
            if (!ir && o.a2 == 0 && o.models > 0) continue;
            r.tones.push_back(std::move(o));
        }
        r.page = j["page"].integer(page);
        r.totalPages = std::max(1, j["total_pages"].integer(1));
        r.ok = true;
        return r;
    }

    ModelList listModels(int toneId, bool ir, int pageSize = 100) {
        ModelList r;
        std::string path = "/api/v1/models?tone_id=" + std::to_string(toneId) + "&page=1&page_size=" +
                           std::to_string(pageSize);
        if (!ir) path += "&architecture=2";
        HttpResp h = authed("GET", path, "", r.err);
        if (!h.ok()) {
            if (r.err.empty()) r.err = errorText(h);
            return r;
        }
        bool ok = false;
        Json j = Json::parse(h.body, &ok);
        if (!ok) {
            r.err = "bad_response";
            return r;
        }
        for (const Json& m : j["data"].items()) {
            ModelInfo o;
            o.id = m["id"].integer();
            o.name = m["name"].str();
            o.url = m["model_url"].str();
            o.size = m["size"].str();
            if (!o.url.empty()) r.models.push_back(std::move(o));
        }
        r.ok = true;
        return r;
    }

    // Downloads m.url (Bearer required) to dest via dest+".part".
    bool download(const ModelInfo& m, const std::string& dest, std::atomic<long long>* bytes,
                  std::atomic<bool>* cancel, std::string& err) {
        std::string access;
        if (!ensureToken(access, err)) return false;
        const std::string part = dest + ".part";
        HttpResp h = http_.download(m.url, {"Authorization: Bearer " + access}, part, bytes, cancel, 300);
        if (h.status == 401) {  // token expired between calls: refresh once
            if (!refreshTokens(err)) return false;
            std::string a2;
            if (!ensureToken(a2, err)) return false;
            h = http_.download(m.url, {"Authorization: Bearer " + a2}, part, bytes, cancel, 300);
        }
        if (cancel && cancel->load()) {
            remove(part.c_str());
            err = "cancelled";
            return false;
        }
        if (!h.ok()) {
            remove(part.c_str());
            err = h.status == 0 ? h.error : "download failed (HTTP " + std::to_string(h.status) + ")";
            return false;
        }
        if (rename(part.c_str(), dest.c_str()) != 0) {
            remove(part.c_str());
            err = "could not write " + dest;
            return false;
        }
        return true;
    }

private:
    IHttp& http_;
    mutable std::mutex mu_;
    std::string access_, refresh_;
    int64_t expiresAt_ = 0;

    static std::string errorText(const HttpResp& h) {
        if (h.status == 0) return h.error.empty() ? "network error" : h.error;
        Json j = Json::parse(h.body);
        std::string e = j["error"].str();
        if (e.empty()) e = j["message"].str();
        return e.empty() ? "HTTP " + std::to_string(h.status) : e + " (HTTP " + std::to_string(h.status) + ")";
    }

    std::string postForm(const std::string& path, const std::vector<std::pair<std::string, std::string>>& kv, long& st,
                         std::string& err) {
        HttpResp h = http_.request("POST", origin + path, {"Content-Type: application/x-www-form-urlencoded"},
                                   formEncode(kv), 30);
        st = h.status;
        if (h.status == 0) err = h.error.empty() ? "network error" : h.error;
        return h.body;
    }

    void setTokens(const std::string& access, const std::string& refresh, int64_t expiresInS) {
        {
            std::lock_guard<std::mutex> g(mu_);
            access_ = access;
            refresh_ = refresh;
            expiresAt_ = now() + expiresInS;
        }
        saveTokens();
    }

    void saveTokens() {
        if (tokenFile.empty()) return;
        std::string a, r;
        int64_t e;
        {
            std::lock_guard<std::mutex> g(mu_);
            a = access_;
            r = refresh_;
            e = expiresAt_;
        }
        const std::string tmp = tokenFile + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << "{\"access\":" << Json::quote(a) << ",\"refresh\":" << Json::quote(r) << ",\"expires_at\":" << e << "}";
        }
        chmod(tmp.c_str(), 0600);
        rename(tmp.c_str(), tokenFile.c_str());
    }

    bool refreshTokens(std::string& err) {
        std::string rt;
        {
            std::lock_guard<std::mutex> g(mu_);
            rt = refresh_;
        }
        if (rt.empty()) {
            err = "not_signed_in";
            return false;
        }
        long st = 0;
        std::string body = postForm("/api/v1/oauth/token",
                                    {{"grant_type", "refresh_token"}, {"refresh_token", rt}, {"client_id", key}}, st, err);
        if (st == 0) return false;
        Json j = Json::parse(body);
        if (j["access_token"].str().empty() || j["refresh_token"].str().empty()) {
            err = "session_expired";
            // a rejected refresh token means we must sign in again
            if (st == 400 || st == 401) signOut();
            return false;
        }
        setTokens(j["access_token"].str(), j["refresh_token"].str(), (int64_t)j["expires_in"].num(3600));
        return true;
    }

    bool ensureToken(std::string& access, std::string& err) {
        bool needRefresh;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (refresh_.empty()) {
                err = "not_signed_in";
                return false;
            }
            needRefresh = access_.empty() || now() >= expiresAt_ - 60;
        }
        if (needRefresh && !refreshTokens(err)) return false;
        std::lock_guard<std::mutex> g(mu_);
        access = access_;
        return true;
    }

    // Authenticated request with one refresh+retry on 401. `err` is set only for
    // auth/transport failures; HTTP errors come back in the response.
    HttpResp authed(const std::string& method, const std::string& path, const std::string& body, std::string& err) {
        HttpResp none;
        std::string access;
        if (!ensureToken(access, err)) return none;
        auto go = [&](const std::string& tok) {
            std::vector<std::string> hdr{"Authorization: Bearer " + tok};
            if (!body.empty()) hdr.push_back("Content-Type: application/json");
            return http_.request(method, origin + path, hdr, body, 30);
        };
        HttpResp r = go(access);
        if (r.status == 401) {
            if (!refreshTokens(err)) return none;
            std::string a2;
            if (!ensureToken(a2, err)) return none;
            r = go(a2);
        }
        return r;
    }
};
