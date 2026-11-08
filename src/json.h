#pragma once
// Tiny JSON reader/writer helpers (enough for the TONE3000 API). No dependencies.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Type type() const { return t_; }
    bool isNull() const { return t_ == Null; }
    bool isObject() const { return t_ == Object; }
    bool isArray() const { return t_ == Array; }
    bool isString() const { return t_ == String; }

    // Lenient accessors: wrong type or missing key gives the default.
    std::string str(const std::string& d = "") const {
        if (t_ == String) return s_;
        if (t_ == Number) return numToString();
        return d;
    }
    double num(double d = 0) const { return t_ == Number ? n_ : (t_ == Bool ? (b_ ? 1 : 0) : d); }
    int integer(int d = 0) const { return t_ == Number ? (int)n_ : d; }
    bool boolean(bool d = false) const { return t_ == Bool ? b_ : d; }
    size_t size() const { return t_ == Array ? a_.size() : (t_ == Object ? o_.size() : 0); }

    const Json& operator[](const std::string& k) const {
        if (t_ == Object) {
            auto it = o_.find(k);
            if (it != o_.end()) return it->second;
        }
        return nullJson();
    }
    const Json& operator[](size_t i) const { return (t_ == Array && i < a_.size()) ? a_[i] : nullJson(); }
    const std::vector<Json>& items() const { return a_; }
    bool has(const std::string& k) const { return t_ == Object && o_.count(k); }

    // Parse; on error returns Null and sets ok=false.
    static Json parse(const std::string& text, bool* ok = nullptr) {
        Parser p{text.c_str(), text.c_str() + text.size()};
        Json v;
        bool good = p.value(v, 0);
        if (good) {
            p.ws();
            good = (p.p == p.e);
        }
        if (ok) *ok = good;
        return good ? v : Json();
    }

    static std::string quote(const std::string& s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        snprintf(b, sizeof b, "\\u%04x", c);
                        o += b;
                    } else o += (char)c;
            }
        }
        return o + "\"";
    }

private:
    Type t_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::map<std::string, Json> o_;

    static const Json& nullJson() {
        static const Json j;
        return j;
    }
    std::string numToString() const {
        char b[40];
        if (n_ == (double)(long long)n_) snprintf(b, sizeof b, "%lld", (long long)n_);
        else snprintf(b, sizeof b, "%g", n_);
        return b;
    }

    struct Parser {
        const char* p;
        const char* e;
        void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
        bool lit(const char* w) {
            size_t n = strlen(w);
            if ((size_t)(e - p) >= n && !strncmp(p, w, n)) { p += n; return true; }
            return false;
        }
        static void utf8(std::string& o, unsigned cp) {
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        }
        bool hex4(unsigned& v) {
            if (e - p < 4) return false;
            v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = *p++;
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else return false;
            }
            return true;
        }
        bool string(std::string& out) {
            if (p >= e || *p != '"') return false;
            ++p;
            while (p < e) {
                char c = *p++;
                if (c == '"') return true;
                if (c != '\\') { out += c; continue; }
                if (p >= e) return false;
                char x = *p++;
                switch (x) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo;
                            if (!hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            }
            return false;
        }
        bool value(Json& v, int depth) {
            if (depth > 64) return false;
            ws();
            if (p >= e) return false;
            char c = *p;
            if (c == '{') {
                ++p;
                v.t_ = Object;
                ws();
                if (p < e && *p == '}') { ++p; return true; }
                for (;;) {
                    ws();
                    std::string k;
                    if (!string(k)) return false;
                    ws();
                    if (p >= e || *p++ != ':') return false;
                    Json child;
                    if (!value(child, depth + 1)) return false;
                    v.o_[k] = std::move(child);
                    ws();
                    if (p >= e) return false;
                    if (*p == ',') { ++p; continue; }
                    if (*p == '}') { ++p; return true; }
                    return false;
                }
            }
            if (c == '[') {
                ++p;
                v.t_ = Array;
                ws();
                if (p < e && *p == ']') { ++p; return true; }
                for (;;) {
                    Json child;
                    if (!value(child, depth + 1)) return false;
                    v.a_.push_back(std::move(child));
                    ws();
                    if (p >= e) return false;
                    if (*p == ',') { ++p; continue; }
                    if (*p == ']') { ++p; return true; }
                    return false;
                }
            }
            if (c == '"') { v.t_ = String; return string(v.s_); }
            if (lit("true")) { v.t_ = Bool; v.b_ = true; return true; }
            if (lit("false")) { v.t_ = Bool; v.b_ = false; return true; }
            if (lit("null")) { v.t_ = Null; return true; }
            char* endp = nullptr;
            double d = strtod(p, &endp);
            if (endp == p || endp > e) return false;
            p = endp;
            v.t_ = Number;
            v.n_ = d;
            return true;
        }
    };
};
