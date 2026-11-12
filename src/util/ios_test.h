// ios (2026-11-13, docs/ios.md "Test modes"): the background flicker's suspects switched on the phone while the game
// runs, so one install tests them all: a tap on the FPS counter (top right) goes to the next mode, its letter shown on
// the counter and written to hotd.log; held half a second, slow motion (a quarter speed) on / off, as the flicker's place goes by fast. A is the build's choice; every other mode changes one suspect, G all of them (as
// on the PC). Documents/args.txt "env:HOTD_IOS_TEST=B" starts in that mode. iOS only.
#pragma once

namespace hotd::iostest {

enum Mode { kA, kB, kC, kD, kE, kF, kG, kModes };

int mode();
void next();
const char* describe();   // "TEST A: ..." (the counter's line)

bool sortPerPixel();      // Arcade Sort's keys by gl_FragDepth (A) or from the vertex depth as on the PC (B, G)
bool zClamp();            // the depth clamp emulated in the vertex shaders (off: C, G)
bool sideOld();           // the side strips in the visfix order, no coverage (D, G)
bool paletteFresh();      // the palette animation in a new texture each frame (off: E, G)
bool sortOff();           // F: Arcade Sort off (the plain per-pixel depth)
bool slow();              // the game at a quarter of its speed (the counter held half a second): time to look
void toggleSlow();

} // namespace hotd::iostest
