// NAM Amp for RG35XX SP / muOS  -  640x480, D-pad driven.
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "engine.h"
#include "http_curl.h"
#include "t3k.h"
#include "wav.h"

namespace fs = std::filesystem;

static const int W = 640, H = 480;

// ------------------------------------------------------------------ config
struct Config {
    std::map<std::string, std::string> kv;
    std::string path;
    void load(const std::string& p) {
        path = p;
        std::ifstream f(p);
        std::string line;
        auto trim = [](std::string t) {  // also strips the \r left by Windows editors
            const char* ws = " \t\r\n";
            size_t a = t.find_first_not_of(ws);
            if (a == std::string::npos) return std::string();
            return t.substr(a, t.find_last_not_of(ws) - a + 1);
        };
        while (std::getline(f, line)) {
            line = trim(line);
            auto eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
            kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    void save() const {
        std::ofstream f(path);
        for (auto& e : kv) f << e.first << "=" << e.second << "\n";
    }
    std::string str(const std::string& k, const std::string& d) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    float num(const std::string& k, float d) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : (float)atof(it->second.c_str());
    }
};

// ------------------------------------------------------------------ text
struct Text {
    SDL_Renderer* r = nullptr;
    TTF_Font* font = nullptr;
    std::map<std::string, std::pair<SDL_Texture*, SDL_Point>> cache;

    bool init(SDL_Renderer* rr, const std::string& appDir) {
        r = rr;
        const std::string cands[] = {appDir + "/font.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                     "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
        for (auto& c : cands) {
            font = TTF_OpenFont(c.c_str(), 20);
            if (font) return true;
        }
        return false;
    }
    void draw(int x, int y, const std::string& s, SDL_Color c, bool right = false, float scale = 1.0f) {
        if (s.empty()) return;
        char key[32];
        snprintf(key, sizeof key, "%02x%02x%02x|", c.r, c.g, c.b);
        std::string k = key + s;
        auto it = cache.find(k);
        if (it == cache.end()) {
            if (cache.size() > 400) clear();
            SDL_Surface* sf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
            if (!sf) return;
            SDL_Texture* t = SDL_CreateTextureFromSurface(r, sf);
            SDL_Point sz{sf->w, sf->h};
            SDL_FreeSurface(sf);
            it = cache.emplace(k, std::make_pair(t, sz)).first;
        }
        const int dw = (int)(it->second.second.x * scale), dh = (int)(it->second.second.y * scale);
        SDL_Rect d{right ? x - dw : x, y, dw, dh};
        SDL_RenderCopy(r, it->second.first, nullptr, &d);
    }
    void clear() {
        for (auto& e : cache) SDL_DestroyTexture(e.second.first);
        cache.clear();
    }
};

// Background job whose result the UI thread picks up when ready.
template <class T>
struct Task {
    std::future<T> f;
    bool busy = false;
    template <class F>
    void run(F fn) {
        f = std::async(std::launch::async, std::move(fn));
        busy = true;
    }
    bool ready() { return busy && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    T take() {
        busy = false;
        return f.get();
    }
};

struct ModelLoad {
    std::shared_ptr<NamModel> m;
    std::string err, path;
};

// ------------------------------------------------------------------ app state
enum Row { R_MODEL, R_SIZE, R_IR, R_SRC, R_AIN, R_AOUT, R_IN, R_GATE, R_BASS, R_MID, R_TREBLE, R_MASTER, R_COUNT };
enum Btn { B_NONE, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_X, B_Y, B_L, B_R };

struct ParamDef {
    const char* label;
    const char* key;
    float mn, mx, step, def;
    std::atomic<float>* v;
};

struct Entry {
    std::string name;
    bool dir = false;
    bool none = false;    // special "no file" entry
    bool online = false;  // special "download from TONE3000" entry
};

struct App {
    Engine eng;
    Config cfg;
    std::string appDir;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    Text txt;

    std::string modelPath, irPath, loopPath;
    bool slimFull = false;  // A2 slimmable tier: false = lite (default on the H700), true = full
    bool modelSlimmable = false;
    Task<ModelLoad> loadTask;
    bool overShown = false;
    // audio device choices
    struct AudioChoice {
        std::string label, dev;  // dev = ALSA name; empty = none
    };
    std::vector<AudioChoice> inChoices, outChoices;
    int inSel = 0, outSel = 0, inActive = 0, outActive = 0;
    int reqRate = 48000;
    bool audioOk = false;
    std::string modelName = "(none)", irName = "(none)", srcName = "Live input";
    std::string status, audioStatus;
    Uint32 statusUntil = 0;
    bool quit = false;

    int sel = 0;
    std::vector<ParamDef> params;

    // browser
    bool browsing = false;
    int browseRow = 0;
    std::string dir;
    std::vector<Entry> entries;
    int bsel = 0, bscroll = 0;
    std::string ext;
    std::string noneLabel;

    // input
    Btn held = B_NONE;
    Uint32 nextRepeat = 0;
    // Defaults for the RG35XX family ("Deeplay-keys" pad): A=3 B=4 Y=5 X=6 L=7 R=8
    int btnA = 3, btnB = 4, btnX = 6, btnY = 5, btnL = 7, btnR = 8;
    std::string lastInput;  // shown on screen to help find button numbers
    bool debugInput = false;
    std::vector<SDL_Joystick*> joys;

    void setStatus(const std::string& s, Uint32 ms = 3000) {
        status = s;
        statusUntil = SDL_GetTicks() + ms;
    }

    int ParamIndex(int row) const { return row - R_IN; }

    // ---------------------------------------------------------- file actions
    static std::string baseName(const std::string& p) { return fs::path(p).filename().string(); }

    void loadModel(const std::string& path) {
        if (path.empty()) {
            eng.setModel(nullptr);
            modelPath.clear();
            modelName = "(bypass)";
            return;
        }
        if (loadTask.busy) {
            setStatus("Still loading the previous model...", 3000);
            return;
        }
        setStatus("Loading model...", 600000);
        const double slim = slimFull ? 1.0 : 0.0;
        const int rate = eng.rate;
        loadTask.run([path, slim, rate]() {  // heavy: runs off the UI thread
            ModelLoad r;
            r.path = path;
            auto m = std::make_shared<NamModel>();
            if (m->load(path, rate, Engine::kMaxBlock, slim, r.err)) r.m = m;
            return r;
        });
    }

    // ---------------------------------------------------------- audio devices
    static std::string shortLabel(const AudioCard& c) { return c.label + " (" + c.dev + ")"; }

    void buildAudioChoices() {
        const std::vector<AudioCard> cards = listAudioCards();
        {  // diagnostic file: what the system sees (readable on the PC)
            std::ofstream f(appDir + "/audio_devices.txt");
            f << "Audio cards found by namsp:\n";
            for (auto& c : cards)
                f << "  " << c.dev << "  " << c.label << (c.capture ? " [in]" : "") << (c.playback ? " [out]" : "") << "\n";
            std::ifstream pc("/proc/asound/cards");
            std::stringstream ss;
            ss << pc.rdbuf();
            f << "\n/proc/asound/cards:\n" << ss.str();
        }
        outChoices.clear();
        outChoices.push_back({"System output (muOS setting)", "default"});
        for (auto& c : cards)
            if (c.playback) outChoices.push_back({shortLabel(c), c.dev});
        inChoices.clear();
        inChoices.push_back({"System input", "default"});
        for (auto& c : cards)
            if (c.capture) inChoices.push_back({shortLabel(c), c.dev});
        inChoices.push_back({"None (loop file only)", ""});

        auto pick = [](std::vector<AudioChoice>& v, const std::string& cur, int& sel) {
            sel = -1;
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i].dev == cur) sel = (int)i;
            if (sel < 0) {
                v.insert(v.begin(), {"Custom: " + cur, cur});
                sel = 0;
            }
        };
        pick(outChoices, eng.playDev, outSel);
        pick(inChoices, eng.capDev, inSel);
        outActive = outSel;
        inActive = inSel;
    }

    void updateAudioStatus(bool ok, const std::string& err) {
        if (ok) {
            char b[200];
            snprintf(b, sizeof b, "Audio %d Hz / %d frames (%.1f ms)%s%s", eng.rate, eng.period,
                     1000.0 * eng.period / eng.rate, eng.hasCapture() ? "" : " - no input",
                     eng.splitClocks() ? "  [2 cards: drift comp.]" : "");
            audioStatus = b;
            if (!eng.hasCapture() && !eng.capDev.empty() && !eng.captureNote.empty())
                setStatus("Input: " + eng.captureNote, 7000);
        } else {
            audioStatus = "AUDIO ERROR: " + err;
            setStatus(audioStatus, 15000);
        }
    }

    void applyAudio() {
        if ((inSel == inActive && outSel == outActive) || inChoices.empty() || outChoices.empty()) return;
        const int oldRate = eng.rate;
        eng.stop();
        eng.capDev = inChoices[(size_t)inSel].dev;
        eng.playDev = outChoices[(size_t)outSel].dev;
        eng.rate = reqRate;
        std::string err;
        audioOk = eng.start(err);
        inActive = inSel;
        outActive = outSel;
        updateAudioStatus(audioOk, err);
        if (audioOk && eng.rate != oldRate) {  // files were prepared for the old rate
            if (!irPath.empty()) loadIR(irPath);
            if (!loopPath.empty()) loadLoop(loopPath);
            if (!modelPath.empty()) loadModel(modelPath);
        }
    }

    std::string srcLabel() const {
        if (!loopPath.empty()) return srcName;
        return eng.hasCapture() ? "Live input" : "Live input (no input device)";
    }

    void loadIR(const std::string& path) {
        if (path.empty()) {
            eng.setIR(nullptr);
            irPath.clear();
            irName = "(none)";
            return;
        }
        WavData w;
        if (!loadWav(path, w, 48000 * 4)) {
            setStatus("IR error: unsupported or unreadable WAV", 5000);
            return;
        }
        auto v = prepareIR(resampleLinear(w.mono, w.rate, eng.rate));
        eng.setIR(std::make_shared<std::vector<float>>(std::move(v)));
        irPath = path;
        irName = baseName(path);
        setStatus("IR loaded", 1500);
    }

    void loadLoop(const std::string& path) {
        if (path.empty()) {
            eng.setLoop(nullptr);
            loopPath.clear();
            srcName = eng.hasCapture() ? "Live input" : "Live input (not available)";
            return;
        }
        setStatus("Loading loop...", 60000);
        render();
        WavData w;
        if (!loadWav(path, w)) {
            setStatus("Loop error: unsupported or unreadable WAV", 5000);
            return;
        }
        eng.setLoop(std::make_shared<std::vector<float>>(resampleLinear(w.mono, w.rate, eng.rate)));
        loopPath = path;
        srcName = "Loop: " + baseName(path);
        setStatus("Loop loaded", 1500);
    }

    // ---------------------------------------------------------- browser
    void openBrowser(int row, const std::string& startDir = "") {
        browseRow = row;
        std::string sub = row == R_MODEL ? "models" : row == R_IR ? "irs" : "loops";
        ext = row == R_MODEL ? ".nam" : ".wav";
        noneLabel = row == R_MODEL ? "[ Bypass amp ]" : row == R_IR ? "[ No cab ]" : "[ Live input ]";
        std::string last = row == R_MODEL ? modelPath : row == R_IR ? irPath : loopPath;
        dir = (!last.empty() && fs::exists(fs::path(last).parent_path())) ? fs::path(last).parent_path().string()
                                                                           : appDir + "/" + sub;
        if (!startDir.empty()) dir = startDir;
        std::error_code ec;
        fs::create_directories(dir, ec);
        refreshEntries();
        browsing = true;
    }

    void refreshEntries() {
        entries.clear();
        entries.push_back({noneLabel, false, true, false});
        if (browseRow != R_SRC) entries.push_back({"[ Get from TONE3000 ]", false, false, true});
        std::error_code ec;
        std::vector<Entry> dirs, files;
        if (fs::path(dir).has_parent_path() && dir != "/") dirs.push_back({"..", true});
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (n.empty() || n[0] == '.') continue;
            if (e.is_directory(ec)) {
                dirs.push_back({n, true});
            } else {
                std::string x = e.path().extension().string();
                std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                if (x == ext) files.push_back({n, false});
            }
        }
        auto cmp = [](const Entry& a, const Entry& b) {
            std::string x = a.name, y = b.name;
            std::transform(x.begin(), x.end(), x.begin(), ::tolower);
            std::transform(y.begin(), y.end(), y.begin(), ::tolower);
            return x < y;
        };
        std::sort(dirs.begin(), dirs.end(), cmp);
        std::sort(files.begin(), files.end(), cmp);
        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), files.begin(), files.end());
        bsel = 0;
        bscroll = 0;
    }

    void browserPress(Btn b) {
        const int visible = 11;
        int n = (int)entries.size();
        switch (b) {
            case B_UP: if (bsel > 0) bsel--; break;
            case B_DOWN: if (bsel < n - 1) bsel++; break;
            case B_L: bsel = std::max(0, bsel - visible); break;
            case B_R: bsel = std::min(n - 1, bsel + visible); break;
            case B_B: browsing = false; break;
            case B_A: {
                Entry& e = entries[(size_t)bsel];
                if (e.online) {
                    openOnline();
                } else if (e.none) {
                    browsing = false;
                    if (browseRow == R_MODEL) loadModel("");
                    else if (browseRow == R_IR) loadIR("");
                    else loadLoop("");
                } else if (e.dir) {
                    dir = (e.name == "..") ? fs::path(dir).parent_path().string() : dir + "/" + e.name;
                    refreshEntries();
                } else {
                    std::string full = dir + "/" + e.name;
                    browsing = false;
                    if (browseRow == R_MODEL) loadModel(full);
                    else if (browseRow == R_IR) loadIR(full);
                    else loadLoop(full);
                }
                break;
            }
            default: break;
        }
        if (bsel < bscroll) bscroll = bsel;
        if (bsel >= bscroll + visible) bscroll = bsel - visible + 1;
    }

    // ---------------------------------------------------------- main screen
    void mainPress(Btn b) {
        if (b == B_UP) sel = (sel + R_COUNT - 1) % R_COUNT;
        else if (b == B_DOWN) sel = (sel + 1) % R_COUNT;
        else if (b == B_Y) quit = true;
        else if (sel == R_SIZE) {
            if (b == B_LEFT || b == B_RIGHT || b == B_A) {
                slimFull = !slimFull;
                if (!modelPath.empty()) loadModel(modelPath);  // tier change = reload (in the background)
                if (!modelPath.empty() && !modelSlimmable) setStatus("This model has a single size", 3000);
            }
        } else if (sel == R_AIN || sel == R_AOUT) {
            auto& v = sel == R_AIN ? inChoices : outChoices;
            int& cur = sel == R_AIN ? inSel : outSel;
            const int n = (int)v.size();
            if (b == B_LEFT && n > 0) cur = (cur + n - 1) % n;
            else if (b == B_RIGHT && n > 0) cur = (cur + 1) % n;
            else if (b == B_A) applyAudio();
            else if (b == B_X) {
                buildAudioChoices();
                setStatus("Audio devices rescanned", 2000);
            }
        } else if (sel < R_IN) {
            if (b == B_A) openBrowser(sel);
        } else {
            ParamDef& p = params[(size_t)ParamIndex(sel)];
            float v = p.v->load();
            if (b == B_LEFT) v -= p.step;
            else if (b == B_RIGHT) v += p.step;
            else if (b == B_L) v -= p.step * 5;
            else if (b == B_R) v += p.step * 5;
            else if (b == B_X) v = p.def;
            p.v->store(std::max(p.mn, std::min(p.mx, v)));
        }
    }

    void press(Btn b) {
        if (b == B_NONE) return;
        if (online != O_NONE) onlinePress(b);
        else if (browsing) browserPress(b);
        else mainPress(b);
    }

    // ---------------------------------------------------------- drawing
    static void fill(SDL_Renderer* r, int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        SDL_Rect rc{x, y, w, h};
        SDL_RenderFillRect(r, &rc);
    }

    static std::string clip(const std::string& s, size_t n) {
        return s.size() <= n ? s : s.substr(0, n - 3) + "...";
    }

    std::string fmtParam(const ParamDef& p) const {
        float v = p.v->load();
        char b[32];
        if (std::string(p.key) == "gate" && v <= -89.0f) return "Off";
        snprintf(b, sizeof b, "%+.1f dB", v);
        return b;
    }

    void render() {
        const SDL_Color bg{18, 18, 22, 255}, panel{34, 34, 42, 255}, hi{255, 140, 30, 255}, wh{235, 235, 235, 255},
            dim{140, 140, 150, 255}, green{80, 200, 110, 255}, yellow{230, 200, 60, 255}, red{230, 70, 70, 255};
        fill(ren, 0, 0, W, H, bg);
        fill(ren, 0, 0, W, 34, panel);
        txt.draw(12, 6, "NAM AMP  -  RG35XX SP", hi);

        if (online != O_NONE) {
            renderOnline();
            SDL_RenderPresent(ren);
            return;
        }

        if (browsing) {
            txt.draw(W - 12, 6, clip(dir, 38), dim, true);
            const int visible = 11, top = 44, rh = 34;
            for (int i = 0; i < visible; ++i) {
                int idx = bscroll + i;
                if (idx >= (int)entries.size()) break;
                int y = top + i * rh;
                if (idx == bsel) fill(ren, 8, y, W - 16, rh - 2, panel);
                const Entry& e = entries[(size_t)idx];
                std::string label = (e.none || e.online) ? e.name : (e.dir ? "[" + e.name + "]" : e.name);
                txt.draw(18, y + 5, clip(label, 52), idx == bsel ? hi : (e.dir || e.none || e.online ? dim : wh));
            }
            if (entries.size() <= (browseRow == R_SRC ? 1u : 2u)) txt.draw(18, top + 40, "No " + ext + " files here. Copy files to: " + clip(dir, 30), dim);
            txt.draw(12, H - 28, "A: select   B: back   L/R: page", dim);
            SDL_RenderPresent(ren);
            return;
        }

        const int top = 44, rh = 26;
        for (int i = 0; i < R_COUNT; ++i) {
            int y = top + i * rh;
            if (i == sel) fill(ren, 8, y, W - 16, rh - 2, panel);
            SDL_Color lc = (i == sel) ? hi : wh;
            if (i == R_SIZE) {
                txt.draw(18, y + 2, "Model size", lc);
                txt.draw(190, y + 2, slimFull ? "Full (heavy, may not run in real time)" : "Lite (recommended)", i == sel ? wh : dim);
            } else if (i == R_AIN || i == R_AOUT) {
                const bool in = i == R_AIN;
                const auto& v = in ? inChoices : outChoices;
                const int cur = in ? inSel : outSel, act = in ? inActive : outActive;
                txt.draw(18, y + 2, in ? "Audio input" : "Audio output", lc);
                std::string nm = v.empty() ? "(none)" : v[(size_t)cur].label;
                if (cur != act) nm += "  (press A)";
                txt.draw(190, y + 2, clip(nm, 40), i == sel ? wh : dim);
            } else if (i < R_IN) {
                const char* lab = i == R_MODEL ? "Amp (NAM)" : i == R_IR ? "Cab (IR)" : "Source";
                std::string nm = i == R_MODEL ? (loadTask.busy ? std::string("Loading...")
                                                               : modelName + (eng.modelBypassed ? "  [BYPASSED: too heavy]" : ""))
                                 : i == R_IR  ? irName
                                              : srcLabel();
                txt.draw(18, y + 2, lab, lc);
                txt.draw(190, y + 2, clip(nm, 36), i == sel ? wh : dim);
            } else {
                const ParamDef& p = params[(size_t)ParamIndex(i)];
                txt.draw(18, y + 2, p.label, lc);
                txt.draw(190, y + 2, fmtParam(p), i == sel ? wh : dim);
                float t = (p.v->load() - p.mn) / (p.mx - p.mn);
                fill(ren, 340, y + 9, 280, 8, {60, 60, 70, 255});
                fill(ren, 340, y + 9, (int)(280 * std::max(0.0f, std::min(1.0f, t))), 8, i == sel ? hi : dim);
            }
        }

        // meters + load
        int my = top + R_COUNT * rh + 8;
        auto meter = [&](int y, const char* lab, float pk) {
            txt.draw(18, y - 4, lab, dim);
            fill(ren, 90, y + 2, 400, 10, {50, 50, 58, 255});
            float db = 20.0f * std::log10(std::max(pk, 1e-5f));
            float t = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
            fill(ren, 90, y + 2, (int)(400 * t), 10, db > -1.0f ? red : (db > -12.0f ? yellow : green));
        };
        meter(my, "IN", eng.peakIn.load());
        meter(my + 22, "OUT", eng.peakOut.load());
        float load = eng.cpuLoad.load();
        SDL_Color lcol = load < 0.6f ? green : (load < 0.85f ? yellow : red);
        char buf[64];
        snprintf(buf, sizeof buf, "DSP %d%%", (int)(load * 100));
        txt.draw(W - 18, my - 4, buf, lcol, true);
        snprintf(buf, sizeof buf, "xruns %d", eng.xruns.load());
        txt.draw(W - 18, my + 18, buf, eng.xruns.load() ? yellow : dim, true);

        bool showStatus = SDL_GetTicks() < statusUntil;
        txt.draw(12, H - 52, showStatus ? status : audioStatus, showStatus ? wh : dim);
        txt.draw(12, H - 26, "A:open  X:reset  L/R:x5  Y:quit", dim);
        if (!lastInput.empty()) txt.draw(W - 12, H - 26, "last input: " + lastInput, dim, true);
        SDL_RenderPresent(ren);
    }


    // ======================================================== TONE3000 online
    CurlHttp http;
    T3kClient t3k{http};
    enum Online { O_NONE, O_SIGNIN, O_LIST, O_MODELS, O_KEYBOARD, O_DOWNLOAD };
    Online online = O_NONE;
    bool onlineIR = false;
    // list
    int tab = 0;  // 0 trending/browse, 1 favourites, 2 search
    std::string query;
    std::vector<Tone> tones;
    int tsel = 0, tscroll = 0, tpage = 1, tpages = 1;
    Task<ToneList> listTask;
    int listGen = 0, listRunning = 0;
    bool reloadPending = false;
    std::string onlineErr;
    // models of the chosen tone
    Tone curTone;
    std::vector<ModelInfo> mlist;
    int msel = 0, mscroll = 0;
    Task<ModelList> modelTask;
    std::string modelsErr;
    // sign-in
    Task<int> authTask;
    std::mutex authMu;
    std::string authCode, authUrl, authMsg;
    std::atomic<bool> authCancel{false};
    // download
    Task<bool> dlTask;
    std::atomic<long long> dlBytes{0};
    std::atomic<bool> dlCancel{false};
    std::atomic<int> dlDone{0}, dlTotal{0};
    std::mutex dlMu;
    std::string dlName, dlErr, dlFolder;
    std::vector<std::string> dlFiles;
    // on-screen keyboard
    std::string kbText;
    int kbR = 0, kbC = 0;
    static constexpr const char* kKeys[4] = {"abcdefghij", "klmnopqrst", "uvwxyz0123", "456789-_. "};

    bool tasksBusy() const { return authTask.busy || listTask.busy || modelTask.busy || dlTask.busy || loadTask.busy; }

    void setAuth(const std::string& code, const std::string& url, const std::string& msg) {
        std::lock_guard<std::mutex> g(authMu);
        authCode = code;
        authUrl = url;
        authMsg = msg;
    }

    void openOnline() {
        onlineIR = (browseRow == R_IR);
        browsing = false;
        onlineErr.clear();
        if (!t3k.hasKey()) {
            setAuth("", "", "no_key");
            online = O_SIGNIN;
            return;
        }
        if (!t3k.signedIn()) {
            startSignIn();
            return;
        }
        online = O_LIST;
        tab = 0;
        tpage = 1;
        loadList();
    }

    void leaveOnline() {
        online = O_NONE;
        browsing = true;  // back to the file list we came from
    }

    void startSignIn() {
        if (authTask.busy) {
            authCancel = true;
            authTask.take();
        }
        online = O_SIGNIN;
        authCancel = false;
        setAuth("", "", "Contacting TONE3000...");
        authTask.run([this]() -> int {
            DeviceAuth da;
            std::string err;
            if (!t3k.startDeviceAuth(da, err)) {
                setAuth("", "", "Could not start sign-in: " + err);
                return 0;
            }
            setAuth(da.userCode, da.verifyUri, "Waiting for you to approve...");
            int waitMs = da.interval * 1000;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(da.expiresIn);
            while (!authCancel) {
                for (int i = 0; i < waitMs / 100 && !authCancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (authCancel) break;
                if (std::chrono::steady_clock::now() > deadline) {
                    setAuth("", "", "The code expired. Press A to get a new one.");
                    return 0;
                }
                std::string r = t3k.pollDevice(da.deviceCode);
                if (r == "ok") return 1;
                if (r == "slow_down") waitMs += 5000;
                else if (r != "authorization_pending") {
                    setAuth("", "", r == "access_denied" ? "Sign-in was declined. Press A to retry."
                                    : r == "expired_token" ? "The code expired. Press A to get a new one."
                                                           : "Sign-in failed: " + r);
                    return 0;
                }
            }
            return 0;
        });
    }

    void loadList() {
        if (tab == 2 && query.empty()) {
            openKeyboard();
            return;
        }
        if (listTask.busy) {
            reloadPending = true;
            return;
        }
        onlineErr.clear();
        tones.clear();
        tsel = tscroll = 0;
        const std::string kind = tab == 1 ? "favorited" : (tab == 2 ? "search" : (onlineIR ? "search" : "trending"));
        const std::string q = tab == 2 ? query : "";
        const bool ir = onlineIR;
        const int page = tpage;
        listRunning = ++listGen;
        listTask.run([this, kind, q, ir, page]() { return t3k.listTones(kind, q, ir, page, 9); });
    }

    void openKeyboard() {
        kbText = query;
        online = O_KEYBOARD;
    }

    void openTone(const Tone& t) {
        curTone = t;
        online = O_MODELS;
        mlist.clear();
        msel = mscroll = 0;
        modelsErr.clear();
        if (modelTask.busy) modelTask.take();
        modelTask.run([this, id = t.id, ir = onlineIR]() { return t3k.listModels(id, ir); });
    }

    static std::string urlExt(const std::string& url, const std::string& dflt) {
        std::string u = url.substr(0, url.find('?'));
        size_t d = u.rfind('.');
        if (d == std::string::npos) return dflt;
        std::string e = u.substr(d);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return (e == ".nam" || e == ".wav") ? e : dflt;
    }

    std::string tonesFolder() const {
        return appDir + "/" + (onlineIR ? "irs" : "models") + "/" + sanitizeFileName(curTone.title);
    }

    std::string destFor(const ModelInfo& m) const {
        std::string name = sanitizeFileName(m.name);
        int same = 0;
        for (auto& o : mlist)
            if (sanitizeFileName(o.name) == name) ++same;
        if (same > 1) name += "_" + std::to_string(m.id);
        return tonesFolder() + "/" + name + urlExt(m.url, onlineIR ? ".wav" : ".nam");
    }

    void startDownload(std::vector<ModelInfo> items) {
        online = O_DOWNLOAD;
        dlCancel = false;
        dlBytes = 0;
        dlDone = 0;
        dlTotal = (int)items.size();
        dlFolder = tonesFolder();
        dlFiles.clear();
        for (auto& m : items) dlFiles.push_back(destFor(m));
        {
            std::lock_guard<std::mutex> g(dlMu);
            dlErr.clear();
            dlName.clear();
        }
        std::error_code ec;
        fs::create_directories(dlFolder, ec);
        auto files = dlFiles;
        dlTask.run([this, items, files]() -> bool {
            for (size_t i = 0; i < items.size(); ++i) {
                {
                    std::lock_guard<std::mutex> g(dlMu);
                    dlName = items[i].name;
                }
                std::error_code e2;
                if (!fs::exists(files[i], e2)) {
                    dlBytes = 0;
                    std::string err;
                    if (!t3k.download(items[i], files[i], &dlBytes, &dlCancel, err)) {
                        std::lock_guard<std::mutex> g(dlMu);
                        dlErr = err;
                        return false;
                    }
                }
                dlDone++;
            }
            return true;
        });
    }

    // Called every frame: collects finished background jobs.
    void pump() {
        if (loadTask.ready()) {
            ModelLoad r = loadTask.take();
            if (!r.m) {
                setStatus("Model error: " + r.err, 6000);
            } else {
                eng.setModel(r.m);
                modelSlimmable = r.m->slimmable();
                modelPath = r.path;
                modelName = baseName(r.path);
                const double er = r.m->expectedRate();
                if (er > 0 && (int)er != eng.rate)
                    setStatus("Warning: model expects " + std::to_string((int)er) + " Hz, running at " + std::to_string(eng.rate), 6000);
                else
                    setStatus("Model loaded", 1500);
            }
        }
        if (eng.modelBypassed.load()) {  // the audio thread gave up on a model that is too heavy
            if (!overShown) {
                overShown = true;
                if (slimFull && modelSlimmable && !loadTask.busy && !modelPath.empty()) {
                    slimFull = false;
                    setStatus("Too heavy: switching to Lite", 6000);
                    loadModel(modelPath);
                } else {
                    setStatus("Model too heavy for real time: bypassed", 8000);
                }
            }
        } else {
            overShown = false;
        }
        if (authTask.ready()) {
            int r = authTask.take();
            if (r == 1 && online == O_SIGNIN) {
                setStatus("Signed in", 2000);
                online = O_LIST;
                tab = 0;
                tpage = 1;
                loadList();
            }
        }
        if (listTask.ready()) {
            ToneList l = listTask.take();
            if (reloadPending) {
                reloadPending = false;
                loadList();
            } else if (listRunning == listGen) {
                if (l.ok) {
                    tones = std::move(l.tones);
                    tpage = l.page;
                    tpages = l.totalPages;
                } else if (l.err == "not_signed_in" || l.err == "session_expired") {
                    if (online == O_LIST) startSignIn();
                } else {
                    onlineErr = l.err;
                }
            }
        }
        if (modelTask.ready()) {
            ModelList m = modelTask.take();
            if (m.ok) mlist = std::move(m.models);
            else modelsErr = m.err;
        }
        if (dlTask.ready()) {
            bool ok = dlTask.take();
            if (ok && online == O_DOWNLOAD) {
                const bool single = dlFiles.size() == 1;
                const std::string first = dlFiles.empty() ? "" : dlFiles[0];
                online = O_NONE;
                if (single) {
                    if (onlineIR) loadIR(first);
                    else loadModel(first);
                } else {
                    openBrowser(onlineIR ? R_IR : R_MODEL, dlFolder);
                    setStatus("Downloaded " + std::to_string(dlFiles.size()) + " files", 4000);
                }
            }
        }
    }

    void onlinePress(Btn b) {
        const int visible = 9;
        switch (online) {
            case O_SIGNIN:
                if (b == B_B) {
                    authCancel = true;
                    leaveOnline();
                } else if (b == B_A && !authTask.busy && t3k.hasKey()) {
                    startSignIn();
                }
                break;
            case O_LIST: {
                const int n = (int)tones.size();
                if (b == B_B) leaveOnline();
                else if (b == B_UP && tsel > 0) tsel--;
                else if (b == B_DOWN && tsel < n - 1) tsel++;
                else if (b == B_L || b == B_R) {
                    tab = (tab + (b == B_R ? 1 : 2)) % 3;
                    tpage = 1;
                    loadList();
                } else if (b == B_LEFT && tpage > 1 && !(tab == 0 && !onlineIR)) {
                    tpage--;
                    loadList();
                } else if (b == B_RIGHT && tpage < tpages && !(tab == 0 && !onlineIR)) {
                    tpage++;
                    loadList();
                } else if (b == B_X) {
                    openKeyboard();
                } else if (b == B_A && n > 0) {
                    openTone(tones[(size_t)tsel]);
                }
                if (tsel < tscroll) tscroll = tsel;
                if (tsel >= tscroll + visible) tscroll = tsel - visible + 1;
                break;
            }
            case O_MODELS: {
                const int base = mlist.size() > 1 ? 1 : 0;
                const int n = (int)mlist.size() + base;
                if (b == B_B) online = O_LIST;
                else if (b == B_UP && msel > 0) msel--;
                else if (b == B_DOWN && msel < n - 1) msel++;
                else if (b == B_A && !mlist.empty()) {
                    if (base == 1 && msel == 0) startDownload(mlist);
                    else startDownload({mlist[(size_t)(msel - base)]});
                }
                if (msel < mscroll) mscroll = msel;
                if (msel >= mscroll + visible) mscroll = msel - visible + 1;
                break;
            }
            case O_DOWNLOAD:
                if (b == B_B) {
                    if (dlTask.busy) dlCancel = true;
                    else online = O_MODELS;
                }
                break;
            case O_KEYBOARD:
                if (b == B_UP) kbR = (kbR + 3) % 4;
                else if (b == B_DOWN) kbR = (kbR + 1) % 4;
                else if (b == B_LEFT) kbC = (kbC + 9) % 10;
                else if (b == B_RIGHT) kbC = (kbC + 1) % 10;
                else if (b == B_A) {
                    if (kbText.size() < 40) kbText += kKeys[kbR][kbC];
                } else if (b == B_X) {
                    if (!kbText.empty()) kbText.pop_back();
                } else if (b == B_B) {
                    online = O_LIST;
                } else if (b == B_Y) {
                    while (!kbText.empty() && kbText.back() == ' ') kbText.pop_back();
                    if (!kbText.empty()) {
                        query = kbText;
                        tab = 2;
                        tpage = 1;
                        online = O_LIST;
                        loadList();
                    }
                }
                break;
            default: break;
        }
    }

    static std::string fmtBytes(long long b) {
        char buf[32];
        if (b >= 1024 * 1024) snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
        else snprintf(buf, sizeof buf, "%lld KB", b / 1024);
        return buf;
    }

    void renderOnline() {
        const SDL_Color bg{18, 18, 22, 255}, panel{34, 34, 42, 255}, hi{255, 140, 30, 255}, wh{235, 235, 235, 255},
            dim{140, 140, 150, 255}, green{80, 200, 110, 255}, red{230, 70, 70, 255};
        fill(ren, 0, 0, W, 34, panel);
        txt.draw(12, 6, std::string("TONE3000  -  ") + (onlineIR ? "Cab IRs" : "Amp models"), hi);

        if (online == O_SIGNIN) {
            std::string code, url, msg;
            {
                std::lock_guard<std::mutex> g(authMu);
                code = authCode;
                url = authUrl;
                msg = authMsg;
            }
            if (msg == "no_key") {
                txt.draw(24, 70, "TONE3000 API key missing", red);
                txt.draw(24, 120, "1. On tone3000.com open Settings > API Keys", wh);
                txt.draw(24, 150, "   and create a publishable key (t3k_pub_...)", dim);
                txt.draw(24, 190, "2. Put this line in namsp.cfg on the SD card:", wh);
                txt.draw(24, 220, "   t3k_key=t3k_pub_xxxxxxxx", hi);
                txt.draw(12, H - 28, "B: back", dim);
                return;
            }
            txt.draw(24, 60, "Sign in to TONE3000", wh);
            if (!code.empty()) {
                txt.draw(24, 110, "On your phone or PC open:", dim);
                txt.draw(24, 140, clip(url, 46), wh);
                txt.draw(24, 190, "and enter this code:", dim);
                txt.draw(24, 220, code, hi, false, 2.6f);
            }
            {
                const SDL_Color mc = authTask.busy ? dim : red;
                for (size_t k = 0, ln = 0; k < msg.size() && ln < 3; k += 50, ++ln)
                    txt.draw(24, 320 + (int)ln * 28, msg.substr(k, 50), mc);
                std::string low = msg;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                auto has = [&](const char* w) { return low.find(w) != std::string::npos; };
                if (!authTask.busy) {
                    if (has("ssl") || has("cert"))
                        txt.draw(24, 410, "Fix: put cacert.pem next to the app (curl.se/ca)", hi);
                    else if (has("resolve") || has("connect") || has("timed out") || has("timeout"))
                        txt.draw(24, 410, "Fix: connect Wi-Fi in the muOS settings", hi);
                }
            }
            txt.draw(12, H - 28, authTask.busy ? "B: cancel" : "A: retry   B: back", dim);
            return;
        }

        if (online == O_KEYBOARD) {
            txt.draw(24, 50, "Search tones", dim);
            fill(ren, 24, 80, W - 48, 40, panel);
            txt.draw(34, 88, kbText + ((SDL_GetTicks() / 400) % 2 ? "_" : " "), wh);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 10; ++c) {
                    const int x = 40 + c * 56, y = 150 + r * 52;
                    const bool on = (r == kbR && c == kbC);
                    fill(ren, x, y, 52, 46, on ? hi : panel);
                    char ch = kKeys[r][c];
                    std::string label = ch == ' ' ? "SP" : std::string(1, ch);
                    txt.draw(x + 26 - (ch == ' ' ? 11 : 6), y + 10, label, on ? bg : wh);
                }
            txt.draw(12, H - 28, "A: type   X: delete   Y: search   B: cancel", dim);
            return;
        }

        if (online == O_DOWNLOAD) {
            std::string name, err;
            {
                std::lock_guard<std::mutex> g(dlMu);
                name = dlName;
                err = dlErr;
            }
            const int total = dlTotal.load(), done = dlDone.load();
            txt.draw(24, 70, clip(curTone.title, 44), wh);
            if (!err.empty()) {
                txt.draw(24, 130, err == "cancelled" ? "Cancelled" : "Download failed", red);
                if (err != "cancelled") txt.draw(24, 165, clip(err, 52), dim);
                txt.draw(12, H - 28, "B: back", dim);
                return;
            }
            txt.draw(24, 130, "Downloading " + std::to_string(std::min(done + 1, total)) + " / " + std::to_string(total), hi);
            txt.draw(24, 165, clip(name, 48), wh);
            txt.draw(24, 200, fmtBytes(dlBytes.load()), dim);
            fill(ren, 24, 250, W - 48, 14, panel);
            fill(ren, 24, 250, (int)((W - 48) * (total ? (float)done / total : 0.0f)), 14, green);
            txt.draw(12, H - 28, "B: cancel", dim);
            return;
        }

        if (online == O_MODELS) {
            txt.draw(W - 12, 6, clip(curTone.user, 20), dim, true);
            txt.draw(24, 44, clip(curTone.title, 46), wh);
            if (modelTask.busy) txt.draw(24, 100, "Loading...", dim);
            else if (!modelsErr.empty()) txt.draw(24, 100, clip(modelsErr, 50), red);
            else if (mlist.empty()) txt.draw(24, 100, "No compatible files for this tone", dim);
            const int base = mlist.size() > 1 ? 1 : 0, n = (int)mlist.size() + base, visible = 9, top = 80, rh = 34;
            for (int i = 0; i < visible; ++i) {
                const int idx = mscroll + i;
                if (idx >= n) break;
                const int y = top + i * rh;
                if (idx == msel) fill(ren, 8, y, W - 16, rh - 2, panel);
                if (base == 1 && idx == 0) {
                    txt.draw(18, y + 5, "[ Download all " + std::to_string(mlist.size()) + " ]", idx == msel ? hi : dim);
                    continue;
                }
                const ModelInfo& m = mlist[(size_t)(idx - base)];
                std::error_code ec;
                const bool have = fs::exists(destFor(m), ec);
                txt.draw(18, y + 5, clip(m.name, 40), idx == msel ? hi : wh);
                txt.draw(W - 18, y + 5, have ? "on SD" : m.size, have ? green : dim, true);
            }
            txt.draw(12, H - 28, "A: download + load   B: back", dim);
            return;
        }

        // O_LIST
        static const char* tabsNam[3] = {"Trending", "Favorites", "Search"};
        static const char* tabsIr[3] = {"Browse", "Favorites", "Search"};
        int x = 12;
        for (int t = 0; t < 3; ++t) {
            std::string lab = (onlineIR ? tabsIr : tabsNam)[t];
            if (t == 2 && !query.empty()) lab += ": " + clip(query, 14);
            fill(ren, x, 38, 8 + (int)lab.size() * 11, 26, t == tab ? hi : panel);
            txt.draw(x + 6, 40, lab, t == tab ? bg : wh);
            x += 20 + (int)lab.size() * 11;
        }
        if (listTask.busy) txt.draw(24, 100, "Loading...", dim);
        else if (!onlineErr.empty()) {
            txt.draw(24, 100, clip(onlineErr, 50), red);
            if (onlineErr.find("certificate") != std::string::npos || onlineErr.find("SSL") != std::string::npos)
                txt.draw(24, 135, "Check the console date/time (Settings).", dim);
        } else if (tones.empty()) txt.draw(24, 100, "Nothing here", dim);
        const int visible = 9, top = 76, rh = 34;
        for (int i = 0; i < visible; ++i) {
            const int idx = tscroll + i;
            if (idx >= (int)tones.size()) break;
            const int y = top + i * rh;
            if (idx == tsel) fill(ren, 8, y, W - 16, rh - 2, panel);
            const Tone& t = tones[(size_t)idx];
            txt.draw(18, y + 5, clip(t.title, 34), idx == tsel ? hi : wh);
            std::string right = clip(t.user, 14);
            if (!onlineIR && t.a2 > 0) right += "  " + std::to_string(t.a2) + " models";
            txt.draw(W - 18, y + 5, right, dim, true);
        }
        char pg[40];
        snprintf(pg, sizeof pg, "page %d/%d", tpage, tpages);
        txt.draw(W - 12, H - 52, pg, dim, true);
        txt.draw(12, H - 28, "A: open  X: search  L/R: tab  </>: page  B: back", dim);
    }

    // ---------------------------------------------------------- input
    Btn mapJoyButton(int b) const {
        if (b == btnA) return B_A;
        if (b == btnB) return B_B;
        if (b == btnX) return B_X;
        if (b == btnY) return B_Y;
        if (b == btnL) return B_L;
        if (b == btnR) return B_R;
        return B_NONE;
    }

    void setHeld(Btn b) {
        held = b;
        if (b != B_NONE) {
            press(b);
            nextRepeat = SDL_GetTicks() + 380;
        }
    }

    void handle(const SDL_Event& e) {
        switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                switch (e.key.keysym.sym) {
                    case SDLK_UP: setHeld(B_UP); break;
                    case SDLK_DOWN: setHeld(B_DOWN); break;
                    case SDLK_LEFT: setHeld(B_LEFT); break;
                    case SDLK_RIGHT: setHeld(B_RIGHT); break;
                    case SDLK_RETURN: press(B_A); break;
                    case SDLK_BACKSPACE: press(B_B); break;
                    case SDLK_x: press(B_X); break;
                    case SDLK_y: press(B_Y); break;
                    case SDLK_PAGEUP: press(B_L); break;
                    case SDLK_PAGEDOWN: press(B_R); break;
                    case SDLK_ESCAPE: quit = true; break;
                    default: break;
                }
                break;
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_LEFT ||
                    e.key.keysym.sym == SDLK_RIGHT)
                    held = B_NONE;
                break;
            case SDL_JOYHATMOTION:
                if (debugInput) printf("hat %d value %d\n", e.jhat.hat, e.jhat.value);
                if (e.jhat.value) lastInput = "hat " + std::to_string((int)e.jhat.value);
                if (e.jhat.value & SDL_HAT_UP) setHeld(B_UP);
                else if (e.jhat.value & SDL_HAT_DOWN) setHeld(B_DOWN);
                else if (e.jhat.value & SDL_HAT_LEFT) setHeld(B_LEFT);
                else if (e.jhat.value & SDL_HAT_RIGHT) setHeld(B_RIGHT);
                else held = B_NONE;
                break;
            case SDL_JOYAXISMOTION: {
                if (debugInput) printf("axis %d value %d\n", e.jaxis.axis, e.jaxis.value);
                if (e.jaxis.axis > 1) break;  // left stick / D-pad-as-axis only
                const int th = 16000;
                Btn nb = B_NONE;
                if (e.jaxis.axis == 0) nb = e.jaxis.value < -th ? B_LEFT : (e.jaxis.value > th ? B_RIGHT : B_NONE);
                else nb = e.jaxis.value < -th ? B_UP : (e.jaxis.value > th ? B_DOWN : B_NONE);
                if (nb != B_NONE && nb != held) setHeld(nb);
                else if (nb == B_NONE && (held == B_LEFT || held == B_RIGHT) == (e.jaxis.axis == 0)) held = B_NONE;
                break;
            }
            case SDL_JOYBUTTONDOWN:
                if (debugInput) printf("button %d\n", e.jbutton.button);
                lastInput = "button " + std::to_string((int)e.jbutton.button);
                press(mapJoyButton(e.jbutton.button));
                break;
            default: break;
        }
    }

    // ---------------------------------------------------------- lifecycle
    void saveConfig() {
        for (auto& p : params) cfg.kv[p.key] = std::to_string(p.v->load());
        cfg.kv["model"] = modelPath;
        cfg.kv["ir"] = irPath;
        cfg.kv["loop"] = loopPath;
        cfg.kv["slim_full"] = slimFull ? "1" : "0";
        cfg.kv["capture"] = eng.capDev;
        cfg.kv["playback"] = eng.playDev;
        cfg.kv["running"] = "0";  // clean exit
        cfg.save();
    }
};

int main(int argc, char** argv) {
    App app;
    char exe[4096] = {0};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    app.appDir = len > 0 ? fs::path(std::string(exe, (size_t)len)).parent_path().string() : ".";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--debug-input") app.debugInput = true;

    Config& cfg = app.cfg;
    cfg.load(app.appDir + "/namsp.cfg");
    // If the previous run never reached a clean exit (freeze, power button), start in safe mode.
    const bool unclean = cfg.str("running", "0") == "1";
    cfg.kv["running"] = "1";
    cfg.save();
    Engine& eng = app.eng;
    eng.capDev = cfg.str("capture", "default");
    eng.playDev = cfg.str("playback", "default");
    eng.rate = (int)cfg.num("rate", 48000);
    app.reqRate = eng.rate;
    eng.period = (int)cfg.num("period", 128);
    app.btnA = (int)cfg.num("btn_a", 3);
    app.btnB = (int)cfg.num("btn_b", 4);
    app.btnX = (int)cfg.num("btn_x", 6);
    app.btnY = (int)cfg.num("btn_y", 5);
    app.btnL = (int)cfg.num("btn_l", 7);
    app.btnR = (int)cfg.num("btn_r", 8);

    app.params = {
        {"Input gain", "in", -12, 24, 0.5f, 0, &eng.p.inGainDb},
        {"Noise gate", "gate", -90, -30, 1, -90, &eng.p.gateDb},
        {"Bass", "bass", -12, 12, 0.5f, 0, &eng.p.bass},
        {"Mid", "mid", -12, 12, 0.5f, 0, &eng.p.mid},
        {"Treble", "treble", -12, 12, 0.5f, 0, &eng.p.treble},
        {"Master", "master", -40, 6, 0.5f, -6, &eng.p.masterDb},
    };
    for (auto& p : app.params) p.v->store(cfg.num(p.key, p.def));
    app.slimFull = !unclean && cfg.num("slim_full", 0) > 0.5f;

    // TONE3000 online access
    curl_global_init(CURL_GLOBAL_DEFAULT);
    app.t3k.key = cfg.str("t3k_key", "");
    app.t3k.origin = cfg.str("t3k_origin", "https://www.tone3000.com");
    app.t3k.tokenFile = app.appDir + "/t3k_auth.json";
    app.t3k.loadTokens();
    {
        std::error_code ec;
        std::string ca = cfg.str("cacert", "");
        if (ca.empty() && fs::exists(app.appDir + "/cacert.pem", ec)) ca = app.appDir + "/cacert.pem";
        app.http.caFile = ca;
        app.http.insecure = cfg.num("tls_insecure", 0) > 0.5f;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    app.win = SDL_CreateWindow("NAM Amp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!app.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    app.ren = SDL_CreateRenderer(app.win, -1, 0);
    if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    if (!app.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(app.ren, W, H);
    if (!app.txt.init(app.ren, app.appDir)) {
        fprintf(stderr, "No font found: put a TTF file named font.ttf next to the executable.\n");
        return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_Joystick* j = SDL_JoystickOpen(i)) app.joys.push_back(j);

    // audio
    std::string err;
    app.audioOk = eng.start(err);
    app.buildAudioChoices();
    app.updateAudioStatus(app.audioOk, err);

    // restore last session (only if audio started, files need the final sample rate)
    if (app.audioOk) {
        auto exists = [](const std::string& p) { std::error_code ec; return !p.empty() && fs::exists(p, ec); };
        std::string m = cfg.str("model", ""), ir = cfg.str("ir", ""), lp = cfg.str("loop", "");
        if (exists(ir)) app.loadIR(ir);
        if (exists(lp)) app.loadLoop(lp);
        if (exists(m) && !unclean) app.loadModel(m);
    }
    if (unclean) app.setStatus("Last session ended abnormally: safe mode (Lite, no model loaded)", 9000);

    while (!app.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) app.handle(e);
        if (app.held != B_NONE && SDL_GetTicks() >= app.nextRepeat) {
            app.press(app.held);
            app.nextRepeat = SDL_GetTicks() + 55;
        }
        app.pump();
        app.render();
        SDL_Delay(25);
    }

    app.saveConfig();
    eng.stop();
    for (auto* j : app.joys) SDL_JoystickClose(j);
    app.txt.clear();
    TTF_CloseFont(app.txt.font);
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(app.win);
    TTF_Quit();
    SDL_Quit();
    curl_global_cleanup();
    if (app.tasksBusy()) {  // a network call is still running: do not wait for it
        app.authCancel = true;
        app.dlCancel = true;
        fflush(stdout);
        _exit(0);
    }
    return 0;
}
