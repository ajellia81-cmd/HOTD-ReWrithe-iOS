// ios (2026-11-12, docs/ios.md "Frame pacing"): where a frame's CPU time goes, on the phone (the first FPS test: cpu
// 33-36 ms a frame, gpu 1-2 ms; a PC: 0.5 ms). A scope adds its time to its slot; util/ios_app.cpp shows the slots on
// the FPS counter and in hotd.log, per frame. Elsewhere the scopes are empty (no cost).
#pragma once

#include <SDL3/SDL_timer.h>

namespace hotd::prof {

enum Slot {
    kTicks,      // the game logic's ticks (Runtime::frame)
    kUnder,      // the arcade's 2D under the 3D (Hud::drawUnder: composed on the CPU, uploaded)
    kView,       // the 3D view (GameView::draw, all of it)
    kSkins,      // inside the view: the skins rebuilt and sent to the GPU (rebuildSkin + Renderer::updateModel)
    kHud,        // the HUD over it (Hud::draw)
    kEsSub,      // the ES layer: buffer data for the emulated texture buffers (uploads)
    kRDraw,      // inside the view: Renderer::draw (the GL part: state, the passes' draw calls)
    kCover,      // inside that: render5's coverage copy (CopyTexSubImage2D of the whole picture mid-frame)
    kBlit,       // the present: our frame's texture blitted into SDL's view (gl_backend's iosPresent)
    kSlots
};

#if defined(HOTD_IOS)
extern double gMs[kSlots];
struct Scope {
    Slot s;
    Uint64 t0;
    explicit Scope(Slot slot) : s(slot), t0(SDL_GetTicksNS()) {}
    ~Scope() { gMs[s] += double(SDL_GetTicksNS() - t0) * 1e-6; }
};
#else
struct Scope {
    explicit Scope(Slot) {}
};
#endif

} // namespace hotd::prof
