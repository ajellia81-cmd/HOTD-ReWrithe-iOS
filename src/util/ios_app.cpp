// ios (2026-11-12): the iPhone / iPad start-up and touch controls (util/ios_app.h, docs/ios.md)
#include "util/ios_app.h"

#include "hotd_version.h"
#include "render/gl.h"
#include "render/gl_backend.h"
#include "render/overlay.h"
#include "util/frame_prof.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <pthread/qos.h>
#include <sstream>
#include <string>
#include <vector>

namespace hotd::prof {
double gMs[kSlots] = {};   // (util/frame_prof.h)
}

namespace hotd::ios {

namespace fs = std::filesystem;

namespace {

std::string trimSlash(std::string s)
{
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string home()
{
    const char* h = std::getenv("HOME");   // (the app's container: Documents, Library, tmp)
    return h && *h ? trimSlash(h) : std::string(".");
}

bool isZip(const fs::path& p)
{
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".zip";
}

std::string firstZip(const fs::path& dir)
{
    std::error_code ec;
    std::vector<std::string> zips;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isZip(e.path())) zips.push_back(e.path().string());
    std::sort(zips.begin(), zips.end());
    return zips.empty() ? std::string() : zips.front();
}

bool hasExe(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && SDL_strcasecmp(e.path().filename().string().c_str(), "THOTD.EXE") == 0) return true;
    return false;
}

// the case-insensitive child of dir named `name` (the Files app keeps what the player typed: "PC", "Pc")
fs::path childNoCase(const fs::path& dir, const char* name)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (SDL_strcasecmp(e.path().filename().string().c_str(), name) == 0) return e.path();
    return dir / name;
}

// the bundle's data/ (CMake puts it at the bundle's root; Resources/data too, for an Xcode project made by hand).
// Under LiveContainer (the player's way to install it) the app runs inside LiveContainer's process: the folder of the
// executable this code is in is looked at too (dladdr), whichever bundle SDL's base path names
fs::path bundleData()
{
    std::vector<fs::path> bases;
    if (const char* b = SDL_GetBasePath()) bases.push_back(fs::path(b));
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&bundleData), &info) && info.dli_fname)
        bases.push_back(fs::path(info.dli_fname).parent_path());
    std::error_code ec;
    for (const fs::path& base : bases)
        for (const fs::path& p : {base / "data", base / "Resources" / "data"})
            if (fs::is_directory(p / "ui", ec) || fs::is_directory(p / "fonts", ec)) return p;
    return {};
}

// the game's own data/ from the bundle into Library/Application Support/rewrithe/data (writable, as a package's
// data/ is), once per build: the stamp beside it is the version and the bundle's file count and bytes
std::string unpackData()
{
    const fs::path root(libraryDir());
    const fs::path from = bundleData();
    std::error_code ec;
    fs::create_directories(root, ec);
    if (from.empty()) {
        SDL_Log("ios: the app has no data folder (bundle %s): the game's fonts and pictures are missing", SDL_GetBasePath());
        return root.string();
    }
    uint64_t files = 0, bytes = 0;
    for (const auto& e : fs::recursive_directory_iterator(from, ec))
        if (e.is_regular_file(ec)) ++files, bytes += uint64_t(e.file_size(ec));
    std::ostringstream st;
    st << HOTD_VERSION_TEXT << "\n" << files << "\n" << bytes << "\n";
    {
        std::ifstream in(root / "data_stamp.txt", std::ios::binary);
        std::stringstream had;
        had << in.rdbuf();
        if (in && had.str() == st.str() && fs::is_directory(root / "data", ec)) return root.string();
    }
    fs::remove_all(root / "data", ec);
    fs::copy(from, root / "data", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) SDL_Log("ios: copying the game's data folder: %s", ec.message().c_str());
    std::ofstream(root / "data_stamp.txt", std::ios::binary | std::ios::trunc) << st.str();
    SDL_Log("ios: the game's data: %llu files from the app into %s", static_cast<unsigned long long>(files), root.string().c_str());
    return root.string();
}

// ---- the life cycle and the touch buttons ----------------------------------------------------------------------------

SDL_Window* gWindow = nullptr;
std::atomic<bool> gBackground{false}, gTerminating{false};
bool gInGame = false;
bool gSwallow = false;             // the touch mouse's press landed on a button: its motion / release are the button's
bool gInjecting = false;           // our own events going in (the filter lets them through)
float gAimX = 0, gAimY = 0;        // the touch mouse's last place in play (window points)
int gReloads = 0, gEscapes = 0;    // presses waiting for beforePresent (no events pushed from inside the filter)
Uint64 gFlashReload = 0, gFlashEsc = 0;

// the frame times (hotd.log every 10 s, and the FPS counter on the screen, temporary: the "frame drops" test). Per
// frame, from the last present's end: sleep = the game's own limiter (SDL_DelayPrecise, main.cpp: delayPrecise), cpu =
// the rest until the frame's commands are sent (the game's logic, building the picture, the driver: a GPU sync in a
// call shows here too), gpu = every 30th frame a glFinish's wait (how far the GPU is behind once the commands are in),
// wait = the display link's tick; the interval: present to present. Slow: over 1.5 beats of the display link
double gFrameProf[prof::kSlots] = {};   // the last frame's parts (prof::gMs, then zeroed)

struct FrameStats {
    Uint64 lastEnd = 0, windowStart = 0, sleepNs = 0;
    int frames = 0, slow = 0, gpuSamples = 0;
    double cpuSum = 0, cpuMax = 0, sleepSum = 0, waitSum = 0, gpuSum = 0, gpuMax = 0, worst = 0;
    double prof[prof::kSlots] = {};   // the frame's CPU by part (util/frame_prof.h), summed
    void work(Uint64 end, Uint64 afterWait, double gpuMs)
    {
        if (!lastEnd) { sleepNs = 0; return; }
        const double sl = double(sleepNs) * 1e-6, w = double(end - lastEnd) * 1e-6 - sl - std::max(0.0, gpuMs);
        cpuSum += w, sleepSum += sl, waitSum += double(afterWait - end) * 1e-6 - std::max(0.0, gpuMs);
        cpuMax = std::max(cpuMax, w);
        if (gpuMs >= 0) gpuSum += gpuMs, gpuMax = std::max(gpuMax, gpuMs), ++gpuSamples;
        sleepNs = 0;
    }
    void gpu(double ms)   // (the Metal present: the GPU's time measured apart from the game thread's)
    {
        if (ms >= 0) gpuSum += ms, gpuMax = std::max(gpuMax, ms), ++gpuSamples;
    }
    bool presented(Uint64 now, const char* what)   // true: a window's numbers ready (fps etc. below)
    {
        for (int k = 0; k < prof::kSlots; ++k) prof[k] += gFrameProf[k];
        if (lastEnd) {
            const double iv = double(now - lastEnd) * 1e-6;
            worst = std::max(worst, iv);
            ++frames;
            const double hz = displayLinkHz() > 1 ? displayLinkHz() : 60.0;
            if (iv > 1.5 * 1000.0 / hz) ++slow;
        }
        lastEnd = now;
        if (!windowStart) windowStart = now;
        return frames > 0 && double(now - windowStart) * 1e-9 >= (what ? 10.0 : 0.5);
    }
    double secs(Uint64 now) const { return double(now - windowStart) * 1e-9; }
    void reset(Uint64 now) { *this = FrameStats{}; lastEnd = windowStart = now; }
};
FrameStats gStats, gShown;   // the 10 s log; the counter's half second
std::string profText(const FrameStats& f)   // the parts' ms a frame
{
    static const char* const kName[prof::kSlots] = {"tick", "2d", "view", "skin", "hud", "esbuf", "gl", "copy", "blit"};
    std::string t;
    char b[32];
    for (int k = 0; k < prof::kSlots; ++k) {
        std::snprintf(b, sizeof b, "%s%s %.1f", t.empty() ? "" : "  ", kName[k], f.prof[k] / std::max(1, f.frames));
        t += b;
    }
    return t;
}
std::vector<std::string> gFpsLines;
bool gFpsChanged = false;

enum Zone { kNone, kReload, kEsc };

bool buttonsShown()
{
    // hidden while a game controller is connected: every touch is the game's (the player's choice, as in their
    // Sega Rally 2 port)
    return gWindow && !SDL_HasGamepad();
}

struct Rect { float x, y, w, h; };

// the buttons' places in window points, inside the safe area (the Dynamic Island, the home indicator)
void zones(Rect& reload, Rect& esc)
{
    int w = 0, h = 0;
    SDL_GetWindowSize(gWindow, &w, &h);
    SDL_Rect safe{0, 0, w, h};
    if (!SDL_GetWindowSafeArea(gWindow, &safe) || safe.w <= 0 || safe.h <= 0) safe = {0, 0, w, h};
    const float sx = float(safe.x), sy = float(safe.y), sr = float(safe.x + safe.w), sb = float(safe.y + safe.h);
    const float rw = float(w) * 0.15f, rh = float(h) * 0.26f;
    reload = {sr - rw, sb - rh, rw, rh};
    const float ew = float(w) * 0.10f, eh = float(h) * 0.17f;
    esc = {sx, sy, ew, eh};
}

Zone hit(float x, float y)
{
    if (!buttonsShown()) return kNone;
    Rect r, e;
    zones(r, e);
    auto in = [&](const Rect& z) { return x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h; };
    if (gInGame && in(r)) return kReload;
    if (in(e)) return kEsc;
    return kNone;
}

void press(Zone z)
{
    if (z == kReload) ++gReloads, gFlashReload = SDL_GetTicks() + 160;
    else if (z == kEsc) ++gEscapes, gFlashEsc = SDL_GetTicks() + 160;
}

bool SDLCALL appFilter(void*, SDL_Event* e)
{
    if (gInjecting) return true;
    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        // no OpenGL ES once in the background (iOS ends an app that draws there): what is queued finished now, the
        // next present waits (beforePresent)
        gBackground = true;
        if (SDL_GL_GetCurrentContext() && gl::Finish) gl::Finish();
        SDL_Log("ios: to the background");
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        gBackground = false;
        SDL_Log("ios: back in the foreground");
        return true;
    case SDL_EVENT_TERMINATING:
        gTerminating = true;
        return true;
    case SDL_EVENT_FINGER_DOWN: {
        // the fingers on the screen now, from SDL's own touch state (the new one already in it): this one alone =
        // SDL's touch mouse (the gun: its button press below is the one tested); more = a second finger. (A count of
        // our own could miss a lift and stay up: every tap a reload)
        int down = 0;
        if (SDL_Finger** f = SDL_GetTouchFingers(e->tfinger.touchID, &down)) SDL_free(f);
        if (down <= 1) return true;
        int w = 0, h = 0;
        SDL_GetWindowSize(gWindow, &w, &h);
        const Zone z = hit(e->tfinger.x * float(w), e->tfinger.y * float(h));
        if (z != kNone) press(z);
        else if (gInGame) press(kReload);   // a second finger anywhere: reload
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.which != SDL_TOUCH_MOUSEID) return true;
        if (const Zone z = hit(e->button.x, e->button.y); z != kNone) {
            press(z);
            gSwallow = true;
            return false;   // (not a shot at the button)
        }
        gAimX = e->button.x, gAimY = e->button.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.which == SDL_TOUCH_MOUSEID && gSwallow) {
            gSwallow = false;
            return false;
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.which != SDL_TOUCH_MOUSEID) return true;
        if (gSwallow) return false;
        gAimX = e->motion.x, gAimY = e->motion.y;
        return true;
    default:
        return true;
    }
}

void push(SDL_Event& ev)
{
    gInjecting = true;
    SDL_PushEvent(&ev);
    gInjecting = false;
}

// the presses as the game's own input: reload = the right button (a shot off the screen) at the gun's place; II /
// BACK = Esc
void sendPresses()
{
    const SDL_WindowID id = gWindow ? SDL_GetWindowID(gWindow) : 0;
    for (; gReloads > 0; --gReloads)
        for (bool down : {true, false}) {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.timestamp = SDL_GetTicksNS();
            ev.button.windowID = id;
            ev.button.which = SDL_TOUCH_MOUSEID;
            ev.button.button = SDL_BUTTON_RIGHT;
            ev.button.down = down;
            ev.button.clicks = 1;
            ev.button.x = gAimX, ev.button.y = gAimY;
            push(ev);
        }
    for (; gEscapes > 0; --gEscapes)
        for (bool down : {true, false}) {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            ev.key.timestamp = SDL_GetTicksNS();
            ev.key.windowID = id;
            ev.key.key = SDLK_ESCAPE;
            ev.key.scancode = SDL_SCANCODE_ESCAPE;
            ev.key.down = down;
            push(ev);
        }
}

void drawButtons(SDL_Window* window)
{
    if (!buttonsShown()) return;
    static Overlay reload, esc;
    static int ready = -1;   // -1 not tried, 0 failed, 1 ok
    if (ready < 0) {
        std::string err;
        ready = reload.init(err) && esc.init(err) ? 1 : 0;
        if (!ready) SDL_Log("ios: the touch buttons' text: %s", err.c_str());
        reload.translate = esc.translate = false;
    }
    if (ready != 1) return;
    int pw = 0, ph = 0, lw = 0, lh = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    SDL_GetWindowSize(window, &lw, &lh);
    if (pw <= 0 || ph <= 0 || lw <= 0) return;
    const float k = float(pw) / float(lw);   // points -> pixels (3 on the Pro Max)
    const int scale = std::max(2, ph / 240);
    const Uint64 now = SDL_GetTicks();
    auto style = [](Overlay& o, bool lit) {
        static const int kNormal = 120, kLit = 220;
        o.setStyle(lit ? kLit : kNormal, 240, lit ? 210 : 235, lit ? 120 : 220);
    };
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);   // (the view's framebuffer: gl_backend's iOS mapping)
    gl::Viewport(0, 0, pw, ph);
    gl::Disable(gl::SCISSOR_TEST);
    gl::Disable(gl::DEPTH_TEST);
    Rect r, e;
    zones(r, e);
    if (gInGame) {
        static bool lit = true;
        if (lit != (now < gFlashReload)) lit = now < gFlashReload, style(reload, lit), reload.setText({"RELOAD"}, scale);
        reload.drawAnchored(pw, ph, (r.x + r.w * 0.55f) * k, (r.y + r.h * 0.6f) * k, 0.5f, 0.5f);
    }
    static int was = -1;
    const int nowEsc = (gInGame ? 2 : 0) + (now < gFlashEsc ? 1 : 0);
    if (was != nowEsc) {
        was = nowEsc;
        style(esc, nowEsc & 1);
        esc.setText({gInGame ? " II " : "BACK"}, scale);
    }
    esc.drawAnchored(pw, ph, (e.x + e.w * 0.45f) * k, (e.y + e.h * 0.45f) * k, 0.5f, 0.5f);
}

// the FPS counter (Options > Video > Show FPS): top right inside the safe area, over everything, pad or not
bool gShowFps = false;
void drawFps(SDL_Window* window)
{
    if (!gShowFps) return;
    static Overlay fps;
    static int ready = -1;
    if (ready < 0) {
        std::string err;
        ready = fps.init(err) ? 1 : 0;
        fps.translate = false;
        fps.setStyle(150, 120, 255, 120);
    }
    if (ready != 1 || gFpsLines.empty()) return;
    int pw = 0, ph = 0, lw = 0, lh = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    SDL_GetWindowSize(window, &lw, &lh);
    if (pw <= 0 || ph <= 0 || lw <= 0) return;
    if (gFpsChanged) fps.setText(gFpsLines, std::max(2, ph / 400)), gFpsChanged = false;
    SDL_Rect safe{0, 0, lw, lh};
    if (!SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0) safe = {0, 0, lw, lh};
    const float k = float(pw) / float(lw);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    gl::Viewport(0, 0, pw, ph);
    gl::Disable(gl::SCISSOR_TEST);
    gl::Disable(gl::DEPTH_TEST);
    fps.drawAnchored(pw, ph, float(safe.x + safe.w) * k - 8.0f, float(safe.y) * k + 8.0f, 1.0f, 0.0f);
}

} // namespace

std::string documentsDir()
{
    if (const char* d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); d && *d) return trimSlash(d);
    return home() + "/Documents";
}

std::string libraryDir()
{
    return home() + "/Library/Application Support/rewrithe";
}

GameFiles findGameFiles()
{
    GameFiles f;
    std::error_code ec;
    const fs::path base(documentsDir());
    // the arcade ROMs: hotd.zip, else the only zip at the top under any name, else roms/ (a zip in it, or the files)
    if (fs::is_regular_file(childNoCase(base, "hotd.zip"), ec)) f.arcade = childNoCase(base, "hotd.zip").string();
    if (f.arcade.empty()) {
        int zips = 0;
        for (const auto& e : fs::directory_iterator(base, ec))
            if (e.is_regular_file(ec) && isZip(e.path())) ++zips, f.arcade = e.path().string();
        if (zips != 1) f.arcade.clear();
    }
    const fs::path roms = childNoCase(base, "roms");
    if (f.arcade.empty() && fs::is_directory(roms, ec)) {
        f.arcade = firstZip(roms);
        if (f.arcade.empty())
            for (const auto& e : fs::recursive_directory_iterator(roms, ec))
                if (e.is_regular_file(ec) && SDL_strcasecmp(e.path().extension().string().c_str(), ".txt") != 0) {
                    f.arcade = roms.string();
                    break;
                }
    }
    // the PC game: pc/ with THOTD.EXE, else a folder one down with it (a folder dropped into pc/), else a zip in pc/
    const fs::path pc = childNoCase(base, "pc");
    if (hasExe(pc)) f.pc = pc.string();
    if (f.pc.empty() && fs::is_directory(pc, ec)) {
        std::vector<std::string> dirs;
        for (const auto& e : fs::directory_iterator(pc, ec))
            if (e.is_directory(ec) && hasExe(e.path())) dirs.push_back(e.path().string());
        std::sort(dirs.begin(), dirs.end());
        if (!dirs.empty()) f.pc = dirs.front();
        if (f.pc.empty()) f.pc = firstZip(pc);
    }
    return f;
}

std::vector<std::string> missingFilesLines(const GameFiles& f)
{
    std::vector<std::string> l;
    l.push_back("The House of the Dead: ReWrithe needs your own game files.");
    l.push_back("");
    l.push_back(std::string("Arcade ROMs (hotd.zip): ") + (f.arcade.empty() ? "MISSING" : "found"));
    l.push_back(std::string("1998 PC game files (THOTD.EXE): ") + (f.pc.empty() ? "MISSING" : "found"));
    l.push_back("");
    // LiveContainer (the app inside LiveContainer's own folder: .../Documents/Data/Application/<id>/Documents): that
    // folder as the Files app shows it; installed on its own: the app's folder in On My iPhone
    const std::string docs = documentsDir();
    const size_t lc = docs.find("/Data/Application/");
    if (lc != std::string::npos) {
        std::string pretty;   // Data > Application > <id> > Documents
        for (char c : docs.substr(lc + 1)) pretty += c == '/' ? std::string(" > ") : std::string(1, c);
        l.push_back("Open the Files app > On My iPhone > LiveContainer >");
        l.push_back(pretty);
    } else {
        l.push_back("Open the Files app > On My iPhone > HOTD ReWrithe");
    }
    l.push_back("Put hotd.zip there, and your PC game's folder in the folder pc");
    l.push_back("");
    l.push_back("Optional: the CD music (Track02.ogg ...) in a folder named Music");
    l.push_back("");
    l.push_back("This page looks for them again every 3 seconds.");
    return l;
}

std::vector<std::string> startArgs(int argc, char** argv)
{
    std::vector<std::string> a;
    for (int i = 0; i < argc; ++i) a.push_back(argv[i] ? argv[i] : "");
    if (a.empty()) a.push_back("hotd");
    auto has = [&](const char* o) { return std::find(a.begin(), a.end(), o) != a.end(); };
    // landscape only (the game is 4:3 / wide); the home indicator dimmed (a swipe shows it, a second one goes home);
    // the touch is the mouse (the gun); a mouse on an iPad stays a mouse
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    const std::string docs = documentsDir();
    std::error_code ec;
    // settings, scores, the log: Documents/settings (the Files app shows them: a bug report's hotd.log)
    if (!std::getenv("HOTD_PREF_DIR")) {
        fs::create_directories(fs::path(docs) / "settings", ec);
        SDL_setenv_unsafe("HOTD_PREF_DIR", (docs + "/settings/").c_str(), 1);
    }
    // the folders the player fills, made now so the Files app shows them
    fs::create_directories(fs::path(docs) / "pc", ec);
    if (FILE* r = std::fopen((docs + "/Put your game files here.txt").c_str(), "wb")) {
        const std::string t = "hotd.zip: your arcade ROM set (MAME's hotd, Revision A)\n"
                              "pc/: your 1998 PC game's files (the folder with THOTD.EXE)\n"
                              "Music/: optional, the CD music (Track02.ogg ...)\n"
                              "settings/: made by the game (settings, scores, hotd.log)\n";
        std::fwrite(t.data(), 1, t.size(), r);
        std::fclose(r);
    }
    const std::string root = unpackData();
    if (!has("--root")) a.insert(a.end(), {"--root", root});
    // (testing: Documents/args.txt, one option per line, adds options; "env:KEY=VALUE" sets an environment variable)
    {
        std::ifstream in(fs::path(docs) / "args.txt");
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.rfind("env:", 0) == 0 && line.find('=') != std::string::npos) {
                const size_t eq = line.find('=');
                SDL_setenv_unsafe(line.substr(4, eq - 4).c_str(), line.substr(eq + 1).c_str(), 1);
            } else if (!line.empty()) a.push_back(line);
        }
        if (in.is_open()) SDL_Log("ios: args.txt read (%zu options now)", a.size());
    }
    const GameFiles f = findGameFiles();
    if (!has("--arcade") && !f.arcade.empty()) a.insert(a.end(), {"--arcade", f.arcade});
    if (!has("--pc") && !f.pc.empty()) a.insert(a.end(), {"--pc", f.pc});
    SDL_Log("ios: Documents %s; arcade %s; PC %s; main thread stack %zu KB", docs.c_str(),
            f.arcade.empty() ? "(missing)" : f.arcade.c_str(), f.pc.empty() ? "(missing)" : f.pc.c_str(),
            pthread_get_stacksize_np(pthread_self()) / 1024);
    return a;
}

void installAppEvents(SDL_Window* window)
{
    gWindow = window;
    static bool once = false;
    if (!once) {
        once = true;
        SDL_SetEventFilter(appFilter, nullptr);
    }
}

void setInGame(bool inGame)
{
    gInGame = inGame;
}

void setShowFps(bool on)
{
    if (on && !gShowFps) gFpsChanged = true;   // (the last numbers drawn again at once)
    gShowFps = on;
}

static bool gMetalPresented = false;
bool presentedByMetal() { return gMetalPresented; }

bool beforePresent(SDL_Window* window)
{
    if (gWindow != window) gWindow = window;
    if (!gBackground) drawButtons(window), drawFps(window);
    sendPresses();
    while (gBackground && !gTerminating) {   // (nothing drawn: the events pumped until the app is back)
        SDL_PumpEvents();
        SDL_Delay(50);
    }
    // SDL's UIKit present shows whichever renderbuffer is bound ("code that binds something else is responsible for
    // rebinding viewRenderbuffer", SDL_uikitopenglview.m): the game's own (its scene targets' depth / colour) would be
    // presented instead of the screen's, and the screen keeps its last good frame (the first device test: the SEGA
    // logo stayed while the attract ran behind it). The view's colour renderbuffer, bound before every present
    // the frame (drawn into a texture of ours: gl_backend's iOS target) into the view's framebuffer
    unsigned metalFbo = 0;
    {
        int pw = 0, ph = 0;
        if (window) SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const prof::Scope ps(prof::kBlit);
        // 120 Hz (util/ios_metal.mm): the frame into a surface Metal shows, once our texture has one
        if (gl::iosSceneReady()) metalFbo = metalTarget(window, pw, ph);
        gl::iosPresent(pw, ph, metalFbo);
    }
    if (metalFbo) {
        // the Metal present: the fence, and the GPU's wait, Metal's copy and present on the present's queue while
        // this thread waits for the display's tick and goes on with the next frame
        gMetalPresented = metalFrameDrawn();
        const Uint64 workEnd = SDL_GetTicksNS();
        waitForDisplay();
        const Uint64 afterWait = SDL_GetTicksNS();
        const double gpuMs = metalGpuMs();   // (measured on the present's queue, beside the game's thread)
        gStats.work(workEnd, afterWait, -1), gStats.gpu(gpuMs);
        gShown.work(workEnd, afterWait, -1), gShown.gpu(gpuMs);
        return !gTerminating;
    }
    gMetalPresented = false;
    static gl::GLuint viewRb = 0;
    if (!viewRb && window)
        viewRb = gl::GLuint(SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0));
    if (viewRb) gl::BindRenderbuffer(gl::RENDERBUFFER, viewRb);
    // vsync (ios_vsync.mm): the frame's commands to the GPU now (it works while we wait), then the display's next tick.
    // Every 30th frame a glFinish first: how long the GPU still needs once the commands are in (the counter's "gpu")
    const Uint64 workEnd = SDL_GetTicksNS();
    static unsigned frameNo = 0;
    double gpuMs = -1;
    if (++frameNo % 30 == 0) {
        gl::Finish();
        gpuMs = double(SDL_GetTicksNS() - workEnd) * 1e-6;
    } else {
        gl::Flush();
    }
    waitForDisplay();
    const Uint64 afterWait = SDL_GetTicksNS();
    gStats.work(workEnd, afterWait, gpuMs);
    gShown.work(workEnd, afterWait, gpuMs);
    return !gTerminating;
}

void afterPresent()
{
    const Uint64 now = SDL_GetTicksNS();
    for (int k = 0; k < prof::kSlots; ++k) gFrameProf[k] = prof::gMs[k], prof::gMs[k] = 0;
    if (gStats.presented(now, "log")) {
        const FrameStats& f = gStats;
        SDL_Log("ios: frames: %d in %.1f s (%.1f fps; the display link %.0f Hz); slow %d (worst %.1f ms); a frame: cpu avg "
                "%.1f ms (max %.1f), gpu %.1f ms (max %.1f, %d samples), the limiter's sleep %.1f ms, the display wait %.1f ms",
                f.frames, f.secs(now), double(f.frames) / f.secs(now), displayLinkHz(), f.slow, f.worst, f.cpuSum / f.frames,
                f.cpuMax, f.gpuSamples ? f.gpuSum / f.gpuSamples : 0.0, f.gpuMax, f.gpuSamples, f.sleepSum / f.frames,
                f.waitSum / f.frames);
        static const auto qosName = [](qos_class_t q) {
            return q == QOS_CLASS_USER_INTERACTIVE ? "user-interactive" : q == QOS_CLASS_USER_INITIATED ? "user-initiated"
                 : q == QOS_CLASS_DEFAULT ? "default" : q == QOS_CLASS_UTILITY ? "utility" : q == QOS_CLASS_BACKGROUND ? "background" : "?";
        };
        SDL_Log("ios: a frame's CPU by part (ms): %s; %s; the game thread's QoS %s", profText(f).c_str(),
                deviceState().c_str(), qosName(qos_class_self()));
        gStats.reset(now);
    }
    if (gShown.presented(now, nullptr)) {
        const FrameStats& f = gShown;
        char a[96], b[96], c[96], g[24] = "-";
        if (f.gpuSamples) std::snprintf(g, sizeof g, "%.1f", f.gpuSum / f.gpuSamples);
        std::snprintf(a, sizeof a, "FPS %.1f  (%.0f Hz)", double(f.frames) / f.secs(now), displayLinkHz());
        std::snprintf(b, sizeof b, "frame %.1f ms  worst %.0f", f.secs(now) * 1000.0 / f.frames, f.worst);
        std::snprintf(c, sizeof c, "cpu %.1f  gpu %s  sleep %.1f", f.cpuSum / f.frames, g, f.sleepSum / f.frames);
        gFpsLines = {a, b, c, profText(f)};
        gFpsChanged = true;
        gShown.reset(now);
    }
}

void delayPrecise(unsigned long long ns)
{
    const Uint64 t0 = SDL_GetTicksNS();
    SDL_DelayPrecise(ns);
    const Uint64 slept = SDL_GetTicksNS() - t0;
    gStats.sleepNs += slept;
    gShown.sleepNs += slept;
}

unsigned defaultFramebuffer(SDL_Window* window)
{
    if (!window) return 0;
    return unsigned(SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
}

} // namespace hotd::ios
