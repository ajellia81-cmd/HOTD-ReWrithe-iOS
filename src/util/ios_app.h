// ios (2026-11-12, docs/ios.md): the iPhone / iPad start-up, the iOS counterpart of util/android_app.h. SDL's UIKit
// app starts SDL_main with no arguments; this gives the game the ones a desktop shortcut would: --root (the game's own
// data/ folder: fonts, ui, lang, typing, mods, copied from the app bundle into Library/Application Support on the first
// start of each build) and the player's game files from the app's Documents folder, which the Files app shows as
// "On My iPhone > HOTD ReWrithe" (and Finder's File Sharing on a computer):
//   Documents/
//     hotd.zip            the arcade ROMs (zip; any one zip at the top too), or roms/ (a zip in it, or the loose files)
//     pc/                 the 1998 PC game's files (THOTD.EXE in pc/ or one folder down, or a zip of them)
//     Music/              optional: the CD music (TrackNN.ogg / .mp3): "the Music folder next to the PC files"
//     settings/           launcher.ini, scores, hotd.log, crash reports (made by the game)
// No game data is in the app.
//
// Touch (the player's iPhone 18 Pro Max: 2868 x 1320, 120 Hz, the Dynamic Island on one side in landscape):
//   a tap              shoots where it lands (SDL's touch-to-mouse: the gun is the finger)
//   RELOAD button      bottom right (in play); a second finger anywhere reloads too
//   II / BACK button   top left: Esc (in play the pause menu, in the menus back)
// The buttons sit inside the safe area and are hidden, leaving every touch to the game, while a game controller is
// connected (they come back when it goes away). iOS only (built there alone).
#pragma once

#include <string>
#include <vector>

struct SDL_Window;

namespace hotd::ios {

std::string documentsDir();   // the app's Documents folder (no trailing slash)
std::string libraryDir();     // Library/Application Support/rewrithe (no trailing slash): the game's own data copy

struct GameFiles {
    std::string arcade, pc;   // empty: not found
};
GameFiles findGameFiles();

// the files page's lines when files are missing (where to put them with the Files app)
std::vector<std::string> missingFilesLines(const GameFiles& f);

// SDL_main's arguments with the iOS defaults added (the given ones win); sets the SDL hints that must come before
// SDL_Init (landscape, the home indicator, touch)
std::vector<std::string> startArgs(int argc, char** argv);

// after the window exists: the event filter for the app's life cycle (no GL in the background) and the touch buttons
void installAppEvents(SDL_Window* window);

// the main loop, every frame: whether the game (not a menu) has the screen (the RELOAD and II buttons, or BACK)
void setInGame(bool inGame);

// before every present: the touch buttons drawn over the frame, their presses sent as the game's input (a right click,
// Esc); while the app is in the background, waits here (events pumped, nothing drawn) until it is back.
// false: the system is ending the app
bool beforePresent(SDL_Window* window);

// right after the present: the frame-time statistics (hotd.log every 10 s: "ios: frames ...") and the FPS counter
void afterPresent();

// the game's frame limiter's sleep (main.cpp's SDL_DelayPrecise on iOS): slept, and counted apart from the frame's work
void delayPrecise(unsigned long long ns);

// the OpenGL ES framebuffer SDL's UIKit view draws into (iOS has no window framebuffer 0)
unsigned defaultFramebuffer(SDL_Window* window);

// vsync (util/ios_vsync.mm): SDL_GL_SetSwapInterval emulated with a CADisplayLink (SDL's UIKit OpenGL ES has none):
// interval n = the display's rate / n; waitForDisplay (in beforePresent): the next tick before the present
bool setSwapInterval(int interval);
void waitForDisplay();
double displayLinkHz();   // the link's measured rate (0: not yet)
std::string deviceState(); // the heat (thermal state) and Low Power Mode, for the log

} // namespace hotd::ios
