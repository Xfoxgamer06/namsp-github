#include "../src/t3k.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unistd.h>

struct Call { std::string method, url, body; std::vector<std::string> headers; };
struct FakeHttp : IHttp {
    std::vector<Call> calls;
    std::function<HttpResp(const Call&)> handler;
    HttpResp request(const std::string& m, const std::string& u, const std::vector<std::string>& h, const std::string& b, int) override {
        calls.push_back({m,u,b,h}); return handler(calls.back()); }
    HttpResp download(const std::string& u, const std::vector<std::string>& h, const std::string& dest,
                      std::atomic<long long>* bytes, std::atomic<bool>* cancel, int) override {
        calls.push_back({"DL",u,dest,h}); HttpResp r = handler(calls.back());
        if (r.ok() && !(cancel && cancel->load())) { FILE* f=fopen(dest.c_str(),"wb"); fwrite(r.body.data(),1,r.body.size(),f); fclose(f); if(bytes) *bytes=(long long)r.body.size(); }
        return r; }
};
static bool has(const Call& c, const std::string& s){ for(auto&h:c.headers) if(h==s) return true; return false; }
static HttpResp R(long st, const std::string& b){ HttpResp r; r.status=st; r.body=b; return r; }

int main() {
    // ---- JSON
    bool ok; Json j = Json::parse(R"({"a":[1,2.5,"x\n\u00e9\ud83c\udfb8"],"b":{"c":true,"d":null},"e":-3e2})", &ok);
    assert(ok && j["a"].size()==3 && j["a"][1].num()==2.5 && j["a"][2].str()=="x\n\xc3\xa9\xf0\x9f\x8e\xb8");
    assert(j["b"]["c"].boolean() && j["b"]["d"].isNull() && j["e"].num()==-300 && j["zz"]["y"].str("def")=="def");
    assert(Json::parse(R"({"a":)", &ok).isNull() && !ok); assert(Json::parse("[1,2] x", &ok).isNull() && !ok);
    assert(Json::parse(R"({"id":42})")["id"].str()=="42");
    Json q = Json::parse(Json::quote("a\"b\\c\n\x01")); assert(q.str()=="a\"b\\c\n\x01");
    // ---- helpers
    assert(urlEscape("a b&c/é")=="a%20b%26c%2F%C3%A9");
    assert(sanitizeFileName("AC30/6: Top*Boost?")=="AC30_6_ Top_Boost_");
    assert(sanitizeFileName("...")=="unnamed" && sanitizeFileName("  x ")=="x");
    assert(sanitizeFileName(std::string(200,'a')).size()==80);
    assert(sanitizeFileName(std::string(79,'a')+"\xc3\xa9").size()==79); // UTF-8 not cut in half

    // ---- client
    system("rm -rf /tmp/t3k_t && mkdir -p /tmp/t3k_t");
    FakeHttp http; T3kClient c(http); c.key="t3k_pub_x"; c.tokenFile="/tmp/t3k_t/auth.json";
    int64_t clock=1000; c.now=[&]{return clock;};
    int polls=0;
    http.handler=[&](const Call& k)->HttpResp{
        if(k.url.find("/oauth/device_authorization")!=std::string::npos) {
            assert(k.body=="client_id=t3k_pub_x");
            return R(200,R"({"device_code":"DC","user_code":"ABCD-1234","verification_uri":"https://t/x","expires_in":600,"interval":2})"); }
        if(k.url.find("/oauth/token")!=std::string::npos) {
            if(k.body.find("device_code")!=std::string::npos && k.body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code")!=std::string::npos){
                ++polls; if(polls==1) return R(400,R"({"error":"authorization_pending"})"); if(polls==2) return R(400,R"({"error":"slow_down"})");
                return R(200,R"({"access_token":"A1","refresh_token":"R1","expires_in":3600})"); }
            if(k.body.find("grant_type=refresh_token")!=std::string::npos){
                if(k.body.find("refresh_token=R1")!=std::string::npos) return R(200,R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
                if(k.body.find("refresh_token=R2")!=std::string::npos) return R(200,R"({"access_token":"A3","refresh_token":"R3","expires_in":3600})");
                return R(400,R"({"error":"invalid_grant"})"); } }
        return R(404,"{}"); };
    DeviceAuth da; std::string err;
    assert(c.startDeviceAuth(da,err) && da.userCode=="ABCD-1234" && da.interval==2 && da.verifyUriComplete=="https://t/x");
    assert(!c.signedIn());
    assert(c.pollDevice("DC")=="authorization_pending" && c.pollDevice("DC")=="slow_down" && c.pollDevice("DC")=="ok");
    assert(c.signedIn());
    // session persisted and reloaded
    { FakeHttp h2; T3kClient c2(h2); c2.tokenFile=c.tokenFile; assert(c2.loadTokens() && c2.signedIn()); }

    // search path + bearer + A2 filtering
    http.calls.clear();
    http.handler=[&](const Call& k)->HttpResp{
        if(k.url.find("/oauth/token")!=std::string::npos) return R(404,"{}");
        if(k.url.find("/tones/search")!=std::string::npos)
            return R(200,R"({"data":[{"id":1,"title":"Plexi","gear":"amp","format":"nam","models_count":5,"a2_models_count":5,"downloads_count":9,"user":{"display_name":"Bob"}},
              {"id":2,"title":"OldA1","gear":"amp","format":"nam","models_count":3,"a2_models_count":0,"user":{"username":"x"}},
              {"id":3,"title":"Cab","gear":"cab","format":"ir","models_count":2,"a2_models_count":0,"user":{"username":"carl"}}],"page":2,"page_size":10,"total_pages":7})");
        return R(404,"{}"); };
    ToneList tl = c.listTones("search","vox ac 30",false,2,10);
    assert(tl.ok && tl.page==2 && tl.totalPages==7 && tl.tones.size()==1); // A1-only and IR tones filtered out of a NAM list
    assert(tl.tones[0].title=="Plexi" && tl.tones[0].user=="Bob");
    assert(http.calls[0].url=="https://www.tone3000.com/api/v1/tones/search?page=2&page_size=10&query=vox%20ac%2030&format=nam&architecture=2");
    assert(has(http.calls[0],"Authorization: Bearer A1"));
    tl = c.listTones("search","",true,1,10); assert(http.calls.back().url.find("&format=ir&architecture=2")!=std::string::npos);
    assert(tl.ok && tl.tones.size()==1 && tl.tones[0].title=="Cab" && tl.tones[0].user=="carl"); // IR list keeps only IR tones
    tl = c.listTones("favorited","",false,3,10); assert(http.calls.back().url=="https://www.tone3000.com/api/v1/tones/favorited?page=3&page_size=10");
    tl = c.listTones("trending","",false,1,10); assert(http.calls.back().url=="https://www.tone3000.com/api/v1/tones/trending");

    // models path: A2 filter for NAM, none for IR
    http.handler=[&](const Call& k)->HttpResp{ if(k.url.find("/models?")!=std::string::npos)
        return R(200,R"({"data":[{"id":11,"name":"Brl 3","model_url":"https://x/11.nam","size":"standard"},{"id":12,"name":"nourl"}]})"); return R(404,"{}"); };
    ModelList ml = c.listModels(101,false); assert(ml.ok && ml.models.size()==1 && ml.models[0].url=="https://x/11.nam");
    assert(http.calls.back().url=="https://www.tone3000.com/api/v1/models?tone_id=101&page=1&page_size=100&architecture=2");
    c.listModels(101,true); assert(http.calls.back().url=="https://www.tone3000.com/api/v1/models?tone_id=101&page=1&page_size=100");

    // 401 -> refresh once -> retry with the new token
    http.calls.clear(); int n=0;
    http.handler=[&](const Call& k)->HttpResp{
        if(k.url.find("/oauth/token")!=std::string::npos) return R(200,R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
        ++n; if(has(k,"Authorization: Bearer A1")) return R(401,R"({"error":"invalid_token"})");
        return R(200,R"({"data":[]})"); };
    tl = c.listTones("favorited","",false,1,10); assert(tl.ok && n==2);
    // expiry (clock jumps past expires_at-60) -> proactive refresh with R2 -> A3
    clock += 100000; http.calls.clear();
    http.handler=[&](const Call& k)->HttpResp{
        if(k.url.find("/oauth/token")!=std::string::npos){ assert(k.body.find("refresh_token=R2")!=std::string::npos); return R(200,R"({"access_token":"A3","refresh_token":"R3","expires_in":3600})"); }
        assert(has(k,"Authorization: Bearer A3")); return R(200,R"({"data":[]})"); };
    assert(c.listTones("favorited","",false,1,10).ok);
    // persisted refresh token rotated
    { FakeHttp h2; T3kClient c2(h2); c2.tokenFile=c.tokenFile; assert(c2.loadTokens()); std::ifstream f(c.tokenFile); std::string s((std::istreambuf_iterator<char>(f)),{}); assert(s.find("\"R3\"")!=std::string::npos); }

    // ---- download
    ModelInfo mi; mi.url="https://x/11.nam"; std::atomic<long long> bytes{0}; std::atomic<bool> cancel{false};
    http.calls.clear();
    http.handler=[&](const Call& k)->HttpResp{ return R(200,"NAMDATA123"); };
    std::string dest="/tmp/t3k_t/out.nam"; assert(c.download(mi,dest,&bytes,&cancel,err));
    assert(access(dest.c_str(),F_OK)==0 && access((dest+".part").c_str(),F_OK)!=0 && bytes==10);
    { std::ifstream f(dest); std::string s; f>>s; assert(s=="NAMDATA123"); }
    // HTTP error leaves no file
    http.handler=[&](const Call&)->HttpResp{ return R(403,"{}"); };
    assert(!c.download(mi,"/tmp/t3k_t/bad.nam",&bytes,&cancel,err) && access("/tmp/t3k_t/bad.nam",F_OK)!=0 && err.find("403")!=std::string::npos);
    // cancel
    cancel=true; http.handler=[&](const Call&)->HttpResp{ return R(200,"X"); };
    assert(!c.download(mi,"/tmp/t3k_t/c.nam",&bytes,&cancel,err) && err=="cancelled" && access("/tmp/t3k_t/c.nam",F_OK)!=0);
    cancel=false;
    // refresh rejected -> signed out, token file removed
    http.handler=[&](const Call& k)->HttpResp{ if(k.url.find("/oauth/token")!=std::string::npos) return R(400,R"({"error":"invalid_grant"})"); return R(401,"{}"); };
    ToneList bad=c.listTones("favorited","",false,1,10); assert(!bad.ok && !c.signedIn() && access(c.tokenFile.c_str(),F_OK)!=0);
    // not signed in
    bad=c.listTones("search","",false,1,10); assert(!bad.ok && bad.err=="not_signed_in");
    puts("T3K ALL OK");
}
