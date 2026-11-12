// ios (2026-11-12): the iPhone / iPad start-up and touch controls (util/ios_app.h, docs/ios.md)
#include "util/ios_app.h"

#include "hotd_version.h"
#include "render/gl.h"
#include "render/overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
std::set<SDL_FingerID> gFingers;   // the fingers down now (the first one is SDL's touch mouse)
bool gSwallow = false;             // the touch mouse's press landed on a button: its motion / release are the button's
bool gInjecting = false;           // our own events going in (the filter lets them through)
float gAimX = 0, gAimY = 0;        // the touch mouse's last place in play (window points)
int gReloads = 0, gEscapes = 0;    // presses waiting for beforePresent (no events pushed from inside the filter)
Uint64 gFlashReload = 0, gFlashEsc = 0;

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
        const bool first = gFingers.empty();
        gFingers.insert(e->tfinger.fingerID);
        if (first) return true;   // (SDL's touch mouse: its button press below is the one tested)
        int w = 0, h = 0;
        SDL_GetWindowSize(gWindow, &w, &h);
        const Zone z = hit(e->tfinger.x * float(w), e->tfinger.y * float(h));
        if (z != kNone) press(z);
        else if (gInGame) press(kReload);   // a second finger anywhere: reload
        return true;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        gFingers.erase(e->tfinger.fingerID);
        return true;
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

bool beforePresent(SDL_Window* window)
{
    if (gWindow != window) gWindow = window;
    if (!gBackground) drawButtons(window);
    sendPresses();
    while (gBackground && !gTerminating) {   // (nothing drawn: the events pumped until the app is back)
        SDL_PumpEvents();
        SDL_Delay(50);
    }
    // SDL's UIKit present shows whichever renderbuffer is bound ("code that binds something else is responsible for
    // rebinding viewRenderbuffer", SDL_uikitopenglview.m): the game's own (its scene targets' depth / colour) would be
    // presented instead of the screen's, and the screen keeps its last good frame (the first device test: the SEGA
    // logo stayed while the attract ran behind it). The view's colour renderbuffer, bound before every present
    static gl::GLuint viewRb = 0;
    if (!viewRb && window)
        viewRb = gl::GLuint(SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0));
    if (viewRb) gl::BindRenderbuffer(gl::RENDERBUFFER, viewRb);
    return !gTerminating;
}

unsigned defaultFramebuffer(SDL_Window* window)
{
    if (!window) return 0;
    return unsigned(SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
}

} // namespace hotd::ios
