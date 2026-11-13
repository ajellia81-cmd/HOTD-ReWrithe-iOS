# iPhone / iPad (iOS)

Worker "ios", 2026-11-12. Goal: the flat game (not VR) on an iPhone, first of all the player's **iPhone 18 Pro Max**
(2868 x 1320 pixels, 2.17:1, 120 Hz, the Dynamic Island on one side in landscape), installed with **LiveContainer**.
**Status: builds in CI (`.github/workflows/ios.yml`); not run on a phone yet.** The desktop builds are unchanged
(everything new is under `HOTD_IOS` / `if(IOS)`).

It follows the Quest port (docs/quest.md): the same OpenGL ES 3.0 layer, the same "copy your game files" page, the
game's own `data/` copied out of the package on the first start of each build.

## Install with LiveContainer

1. Get the `.ipa`: the repository's release **latest** always has the newest build as `HOTD-ReWrithe-iOS.ipa`
   (`https://github.com/<owner>/<repo>/releases/download/latest/HOTD-ReWrithe-iOS.ipa`). Or build it on a Mac:
   `ios/build.sh` (below).
2. LiveContainer -> **+** -> choose the `.ipa`. It is unsigned on purpose: LiveContainer signs it itself.
3. Start it once: it shows **"needs your own game files"** with the folder to use, and makes that folder.
4. **Files** app -> On My iPhone -> **LiveContainer** -> `Data` -> `Application` -> the app's folder -> `Documents`
   (the page on the screen shows this exact path):
   - `hotd.zip` (your MAME `hotd` ROM set, Revision A) right there,
   - your 1998 PC game's folder (the one with `THOTD.EXE`) into `pc`, or a zip of it into `pc`,
   - optional: the CD music (`Track02.ogg` ... or `.mp3`) in a folder `Music`.
   Within 3 seconds the game finds them and goes on to the files check and the menu.
5. A bug report: `Documents/settings/hotd.log` (and `crash-<date>.txt` after a crash), never game files.

Installed on its own (Xcode, AltStore, SideStore), the folder is Files -> On My iPhone -> **HOTD ReWrithe**.

## Controls

| | |
|---|---|
| tap | shoot where it lands (the finger is the gun; SDL's touch-to-mouse) |
| **RELOAD** (bottom right, in play) | reload (as a shot off the screen) |
| a second finger anywhere (in play) | reload too |
| **II** (top left, in play) / **BACK** (in the menus) | Esc: the pause menu / back |
| a game controller | **left stick** moves the crosshair, **R2 / RT** shoots, **L2 / LT** reloads, **A (South) held** = the crosshair 2.5x faster; Start pauses. It works as soon as it connects (player 1 keeps the touch too, nothing to set); **the touch buttons hide** while one is connected and come back when it goes away (no pause). The gyro is not used unless a motion style is chosen in the controller options |
| an iPad's mouse / keyboard | as on a PC |

The buttons sit inside the screen's safe area (clear of the Dynamic Island and the home indicator). Options ->
Gameplay -> **Auto Reload** reloads an empty gun by itself, if you prefer no button.

**Frame pacing:** SDL's UIKit OpenGL ES has no vsync (swap interval), so at first the game's frames went out on its
limiter's own clock, not the display's: some refreshes showed a frame twice and the next one skipped, "frame drops"
while the game itself kept its rate. `util/ios_vsync.mm` now runs a CADisplayLink on a thread of its own and emulates
`SDL_GL_SetSwapInterval` (interval n: the display's rate / n); each present flushes the frame to the GPU and waits for
the next tick. hotd.log every 10 s: `ios: frames: N in 10 s (fps; the display link's real rate); slow N (worst ms);
the frame's work avg / max ms; the wait for the display ms` (work near the frame's whole time: the game or a GPU sync is
the limit; slow frames with a small work: the display side).

**The coverage copy (the frame drops' cause):** the FPS counter's split (`util/frame_prof.h`) showed a frame's CPU
time almost all in the 3D view, and there in render5's coverage copy (`Renderer::draw`: CopyTexSubImage2D of the
whole 2868 x 1320 picture in the middle of the frame for the widescreen side strips): 21-23 ms of 26-29 ms, the GPU
~1 ms; out of a texture of ours still ~6 ms. Skipping it (the visfix order) made a window at the picture's edge
flicker. iOS now draws every frame into a texture of its own (`gl_backend`'s `iosPresent`: "framebuffer 0" is that
texture's, blitted into SDL's view at the present) and writes the zone's coverage as a second colour output while the
zone draws (the fragment shader's `oCover`, `coverTex_` attached as colour 1, detached before the pieces read it):
no copy at all. Into SDL's own view (`HOTD_IOS_DIRECT=1`) the copy as before.

**The HUD's uploads:** with the copy gone the counter showed "hud" 14-15 ms of a 17 ms frame: `Hud`'s one texture
re-specified several times a frame (the 2D layer, its sides, the fills, the fades) made Apple's OpenGL ES wait for
the GPU at each. On iOS each upload takes the next of a ring of 48 textures (`Hud::freshTexture`).

**Texture uploads:** the same wait elsewhere: the palette animation's texture (every frame) and the overlays' text
(the FPS counter twice a second) get a new texture for new contents on iOS, the old one deleted (freed once the frames
in flight are done). The frame's depth / stencil are invalidated before the blit (not stored back on the tile GPU).

**Depth clamp (the edge flicker):** Apple's OpenGL ES has no GL_EXT_depth_clamp ("OpenGL ES: depth clamp
unavailable" in the log), so the near and far planes clipped what `Renderer::draw` keeps on the PC ("no near clip, as
the Model 2"): a wall or window near the camera cut open from frame to frame. The player's A/B tests
(`HOTD_IOS_DIRECT=1`, `HOTD_SIDE_OLD=1`) flickered both, so not the side strips' cover. Emulated in every vertex
shader on iOS (`esShaderSource`): main wrapped, clip-space z kept within -w..w in front of the eye.
`HOTD_IOS_NO_ZCLAMP=1`: off (the flicker stayed). **Depth ties (the edge flicker, 2):** Apple's GPUs have no 24-bit
depth buffer, so DEPTH24 is a 32-bit float there; the PC's earlyz path (Arcade Sort's keys from the vertex depth) needs
the rounding to 24-bit steps to make two polygons with one key tie exactly, so on the phone float noise decided them per
pixel (a window and the wall behind it in turn). iOS writes the keys per pixel instead (`gl_FragDepth`, the depthcmp
path); `HOTD_IOS_EARLYZ=1`: as on the PC. **The 120 Hz ceiling:** with every wait gone a frame's work is 3-4 ms, yet the presents
stay at exactly 60 a second, the rest of the frame spent in the present's blit: the OpenGL ES layer is shown at 60 Hz
here (the display link itself runs at 120). **120 Hz (`util/ios_metal.mm`):** the frame still drawn by OpenGL ES into our texture, then blitted (flipped) into
one of three IOSurfaces that OpenGL ES and Metal share; a fence, the display link's tick, and Metal copies the surface
into a CAMetalLayer's drawable (a view over SDL's, no touches) and presents it, SDL_GL_SwapWindow skipped. CAMetalLayer
runs at the display's whole rate. Nothing for Metal (no device, no texture cache): the OpenGL ES present at 60 as before;
`HOTD_IOS_GLPRESENT=1` the same (comparisons). The log: "ios: frames shown by Metal".

**Show FPS** (Options > Video, iOS only, off by default): the frame-time counter at the top right (fps, the
frame's ms, CPU / GPU, the CPU by part). **Frame rate:** 60 fps by default on iOS (Options > Video > Frame Rate; Display Refresh for 120 where LiveContainer
lets frames faster than 60 through). The first device test at 120 paced unevenly.

## What is different on iOS

- **Screen:** always the whole display at its own pixels (`SDL_WINDOW_HIGH_PIXEL_DENSITY`: 2868 x 1320 on the Pro Max,
  not the 956 x 440 points), landscape only, no status bar; the home indicator is dimmed (swipe twice to leave).
  The menus are 4:3 with side bars as on any wide screen; the 3D view fills the width.
- **120 Hz:** `CADisableMinimumFrameDurationOnPhone` is in the app's Info.plist. Under LiveContainer the host app's
  Info.plist decides: if the game stays at 60 fps there, that is LiveContainer's setting, not the game's.
- **Graphics:** Apple's OpenGL ES 3.0 (deprecated by Apple but still in iOS) through SDL's UIKit view, the same ES layer
  as ANGLE and the Quest (`render/gl_backend.cpp`). One iOS-only addition: the view draws into its own framebuffer
  object, so the game's framebuffer 0 (the window) is mapped to it (`BindFramebuffer`).
- **The background:** iOS ends an app that draws while in the background. When the app leaves the foreground the
  queued GL work is finished and the next frame waits (events pumped, nothing drawn) until it is back
  (`ios::beforePresent`); the game loop drops the stall instead of fast-forwarding (as after a Steam Deck suspend).
- **Left out of Options:** Display Mode, Display, Resolution, Renderer, VRR, Vsync (always the display's), Edge Reload
  (a finger cannot leave the screen), the light guns' rows, Mouse Sensitivity, Player 1-4 Input (player 1 takes the
  touch and a pad at once), The Typing of the Dead 0 and Trailer Mode (a keyboard), Online Co-op, Repair / Change
  Files (the Files app instead), Check for Updates, Desktop Shortcut, Add to Steam. The pad's crosshair speed: Controls
  > Controller: Player 1 > Stick Sensitivity.
- **Not on iOS:** the update check (a sideloaded app), text-to-speech (no programs to start), the developer tools.
- **Files:** settings, scores and the log in `Documents/settings` (so the Files app shows them); the game's own
  `data/` (fonts, menu pictures, languages, mods) copied from the app into `Library/Application Support/rewrithe` on
  the first start of each build (about 30 MB, a second or two).
- **Testing:** `Documents/args.txt`, one command-line option per line (`env:KEY=VALUE` sets a variable), as the Quest's.

## Building

On a Mac with Xcode (15 or newer), CMake 3.21+, Python 3 and git:

```
ios/build.sh                 # fetches the pinned third-party sources once, builds, packages
```

Result: `build-ios/ipa/HOTD-ReWrithe-ios-v<version>.ipa` (unsigned, arm64, iOS 15+). The script fetches the same pinned
sources as the release workflow (SDL 3.2.10 built statically for iOS, stb_vorbis, dr_mp3, Musashi, MAME's SCSP), builds
Musashi's `m68kmake` for the Mac and runs it (a cross build cannot run its own; `HOTD_M68KOPS_DIR`), configures with
`-G Xcode -DCMAKE_SYSTEM_NAME=iOS`, builds the `hotd` target as `HOTDReWrithe.app` with code signing off, adds the
home screen icon (the game's own neutral icon, `src/res/hotd.ico`), checks the bundle (arm64, `data/` in it, no game
data) and zips `Payload/HOTDReWrithe.app` into the `.ipa`.

Signed by Xcode instead (a free Apple ID works for 7 days): configure with `-DHOTD_IOS_SIGN=ON`, open
`build-ios/hotd_m2.xcodeproj`, choose your team on the `hotd` target, run it on the phone.

CI: `.github/workflows/ios.yml` runs `ios/build.sh` on `macos-latest` on a push to `ios` / `main`, a version tag or
by hand, and puts the `.ipa` (no zip) on the release `latest` under a fixed name.

## Code

| file | what |
|---|---|
| `src/util/ios_app.h/.cpp` | the start-up (`--root`, the Documents folder, the files page's text), the touch buttons, the background wait, the view's framebuffer |
| `src/main.cpp` | `HOTD_IOS` blocks beside the Quest's: the start arguments, the files page, the window flags, `setInGame`, the present |
| `src/render/gl_backend.cpp` | OpenGL ES 3.0 always; framebuffer 0 = the UIKit view's |
| `src/game/menu_pages.cpp` | the rows iOS leaves out |
| `src/util/tts.cpp`, `src/util/steam_shortcut.cpp` | no process spawning / libproc on iOS |
| `CMakeLists.txt` | `if(IOS)`: the app bundle, static SDL3, the frameworks, `data/` copied into the bundle, no tools |
| `ios/Info.plist.in`, `ios/build.sh`, `.github/workflows/ios.yml` | the bundle's Info.plist, the build, the CI |

## To check on the phone

- The first start: the files page, then the menu after copying the files (LiveContainer's folder).
- Chapter 1 with touch: shots land under the finger, RELOAD / second-finger reload, II pauses, BACK in the menus.
- A controller connected / disconnected: the buttons hide / come back.
- To the home screen and back during play: no crash, the game continues.
- 120 Hz in LiveContainer (Options -> Video -> Frame Rate), the arcade sound board's CPU time on the phone.
- `hotd.log`'s `ios:` lines (the Documents path, the main thread's stack, the view's framebuffer).
