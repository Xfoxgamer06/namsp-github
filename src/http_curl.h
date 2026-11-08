#pragma once
// libcurl implementation of IHttp. Every call uses its own easy handle, so it is
// safe to use from several threads (call curl_global_init() once at startup).
#include <curl/curl.h>

#include <cstdio>
#include <string>
#include <vector>

#include "t3k.h"

class CurlHttp : public IHttp {
public:
    std::string caFile;     // path to a CA bundle (cacert.pem); empty = system default
    bool insecure = false;  // last resort: skip TLS verification (config tls_insecure=1)

    HttpResp request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                     const std::string& body, int timeoutSec) override {
        HttpResp r;
        CURL* c = curl_easy_init();
        if (!c) { r.error = "curl init failed"; return r; }
        curl_slist* hl = nullptr;
        for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
        setup(c, url, hl, timeoutSec);
        if (method == "POST") {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
            curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body.c_str());
        } else if (method != "GET") {
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
            if (!body.empty()) {
                curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
                curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body.c_str());
            }
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &CurlHttp::writeString);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        CURLcode rc = curl_easy_perform(c);
        finish(c, rc, r);
        curl_slist_free_all(hl);
        curl_easy_cleanup(c);
        return r;
    }

    HttpResp download(const std::string& url, const std::vector<std::string>& headers, const std::string& destPath,
                      std::atomic<long long>* bytes, std::atomic<bool>* cancel, int timeoutSec) override {
        HttpResp r;
        FILE* f = fopen(destPath.c_str(), "wb");
        if (!f) { r.error = "cannot write " + destPath; return r; }
        CURL* c = curl_easy_init();
        if (!c) { fclose(f); r.error = "curl init failed"; return r; }
        curl_slist* hl = nullptr;
        for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
        setup(c, url, hl, timeoutSec);
        DlCtx ctx{f, bytes, cancel};
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);  // HTTP >= 400 -> error, no body written
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &CurlHttp::writeFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &CurlHttp::progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
        CURLcode rc = curl_easy_perform(c);
        finish(c, rc, r);
        fclose(f);
        if (!r.ok()) remove(destPath.c_str());
        curl_slist_free_all(hl);
        curl_easy_cleanup(c);
        return r;
    }

private:
    struct DlCtx {
        FILE* f;
        std::atomic<long long>* bytes;
        std::atomic<bool>* cancel;
    };
    static size_t writeString(char* p, size_t s, size_t n, void* ud) {
        static_cast<std::string*>(ud)->append(p, s * n);
        return s * n;
    }
    static size_t writeFile(char* p, size_t s, size_t n, void* ud) {
        auto* d = static_cast<DlCtx*>(ud);
        size_t w = fwrite(p, s, n, d->f);
        if (d->bytes) d->bytes->fetch_add((long long)w);
        return w;
    }
    static int progress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* d = static_cast<DlCtx*>(ud);
        return (d->cancel && d->cancel->load()) ? 1 : 0;  // non-zero aborts the transfer
    }
    void setup(CURL* c, const std::string& url, curl_slist* hl, int timeoutSec) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "namsp/1.0 (muOS)");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // curl drops Authorization on cross-host redirects
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeoutSec);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        if (!caFile.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, caFile.c_str());
        if (insecure) {
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }
    static void finish(CURL* c, CURLcode rc, HttpResp& r) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (rc == CURLE_OK) { r.status = code; return; }
        if (rc == CURLE_HTTP_RETURNED_ERROR) { r.status = code; return; }  // download() with FAILONERROR
        r.status = 0;
        r.error = curl_easy_strerror(rc);
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_SSL_CERTPROBLEM)
            r.error += " (check the console date/time and cacert.pem)";
    }
};
