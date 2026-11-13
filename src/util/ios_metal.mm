// ios (2026-11-13, docs/ios.md "120 Hz"): the frames shown through Metal. Apple's OpenGL ES layer (CAEAGLLayer, SDL's
// UIKit view) is shown at 60 Hz at most on the iPhone's ProMotion screen: with a frame's work at 3-4 ms and the display
// link at 120 the presents stayed at exactly 60 a second. The game still draws with OpenGL ES (its frame in a texture
// of ours, gl_backend's iosPresent); here that frame is blitted into one of three IOSurfaces shared by OpenGL ES and
// Metal, and Metal copies it into a CAMetalLayer's drawable over SDL's view and presents it (on a queue of its own once
// the GPU is done: below) (CAMetalLayer: the
// display's full rate). SDL_GL_SwapWindow is skipped then (main.cpp vrSwap). Anything missing (no Metal, a texture
// cache that cannot be made): the OpenGL ES present as before. HOTD_IOS_GLPRESENT=1: the old present (comparisons).
#include "util/ios_app.h"

#define GLES_SILENCE_DEPRECATION 1
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>

@interface HotdMetalView : UIView
@end
@implementation HotdMetalView
+ (Class)layerClass
{
    return [CAMetalLayer class];
}
@end

namespace {

constexpr int kSlots = 3;
constexpr GLenum kBgraExt = 0x80E1;   // GL_BGRA_EXT (APPLE_texture_format_BGRA8888)

struct Slot {
    CVPixelBufferRef pb = nullptr;
    CVOpenGLESTextureRef glTex = nullptr;
    CVMetalTextureRef mtlTex = nullptr;
    GLuint fbo = 0;
};

bool gTried = false, gOn = false;
id<MTLDevice> gDevice = nil;
id<MTLCommandQueue> gQueue = nil;
CAMetalLayer* gLayer = nil;
HotdMetalView* gView = nil;
CVOpenGLESTextureCacheRef gGlCache = nullptr;
CVMetalTextureCacheRef gMtlCache = nullptr;
dispatch_semaphore_t gInFlight = nullptr;
Slot gSlot[kSlots];
int gW = 0, gH = 0, gCur = 0;
bool gPending = false;   // a slot handed out, waiting for metalFrameDrawn
// the present's own queue (2026-11-13, the first 120 Hz test: frames dropped while "gpu" was 6-10 ms): the game's
// thread waited for OpenGL ES's GPU work before every present, so a frame took its CPU time plus its GPU time, one
// after the other. Now the wait and Metal's copy and present run on this queue (a second OpenGL ES context in the same
// share group waits for the frame's fence), and the game's thread goes on with the next frame while the GPU works
dispatch_queue_t gPresentQ = nullptr;
EAGLContext* gWaitCtx = nil;
std::atomic<double> gGpuSum{0};
std::atomic<int> gGpuN{0};

void freeSlots()
{
    for (Slot& s : gSlot) {
        if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
        if (s.glTex) CFRelease(s.glTex);
        if (s.mtlTex) CFRelease(s.mtlTex);
        if (s.pb) CVPixelBufferRelease(s.pb);
        s = Slot{};
    }
    if (gGlCache) CVOpenGLESTextureCacheFlush(gGlCache, 0);
    if (gMtlCache) CVMetalTextureCacheFlush(gMtlCache, 0);
}

bool makeSlots(int w, int h)
{
    GLint prevFbo = 0, prevTex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    NSDictionary* attrs = @{
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString*)kCVPixelBufferOpenGLESCompatibilityKey: @YES,
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    bool ok = true;
    for (Slot& s : gSlot) {
        if (CVPixelBufferCreate(kCFAllocatorDefault, size_t(w), size_t(h), kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)attrs, &s.pb) != kCVReturnSuccess) { ok = false; break; }
        if (CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, gGlCache, s.pb, nullptr, GL_TEXTURE_2D,
                GL_RGBA, w, h, kBgraExt, GL_UNSIGNED_BYTE, 0, &s.glTex) != kCVReturnSuccess) { ok = false; break; }
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, gMtlCache, s.pb, nullptr,
                MTLPixelFormatBGRA8Unorm, size_t(w), size_t(h), 0, &s.mtlTex) != kCVReturnSuccess) { ok = false; break; }
        const GLuint tex = CVOpenGLESTextureGetName(s.glTex);
        glBindTexture(CVOpenGLESTextureGetTarget(s.glTex), tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = false; break; }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prevFbo));
    glBindTexture(GL_TEXTURE_2D, GLuint(prevTex));
    if (!ok) {
        freeSlots();
        return false;
    }
    gW = w, gH = h;
    gLayer.drawableSize = CGSizeMake(w, h);
    return true;
}

bool start(SDL_Window* window)
{
    gTried = true;
    if (std::getenv("HOTD_IOS_GLPRESENT")) {
        SDL_Log("ios: frames shown by OpenGL ES (HOTD_IOS_GLPRESENT)");
        return false;
    }
    EAGLContext* ctx = [EAGLContext currentContext];
    UIWindow* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                     SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIView* host = uiWindow.rootViewController.view;
    gDevice = MTLCreateSystemDefaultDevice();
    if (!ctx || !host || !gDevice) {
        SDL_Log("ios: no Metal present (context %d, view %d, device %d): OpenGL ES's (60 Hz)", ctx != nil, host != nil,
                gDevice != nil);
        return false;
    }
    if (CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, ctx, nullptr, &gGlCache) != kCVReturnSuccess ||
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, gDevice, nullptr, &gMtlCache) != kCVReturnSuccess) {
        SDL_Log("ios: no texture caches for a Metal present: OpenGL ES's (60 Hz)");
        return false;
    }
    gQueue = [gDevice newCommandQueue];
    gView = [[HotdMetalView alloc] initWithFrame:host.bounds];
    gView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    gView.userInteractionEnabled = NO;   // (every touch still SDL's view's)
    gView.opaque = YES;
    gLayer = (CAMetalLayer*)gView.layer;
    gLayer.device = gDevice;
    gLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    gLayer.framebufferOnly = NO;   // (a blit's destination)
    gLayer.opaque = YES;
    gLayer.contentsScale = uiWindow.screen.nativeScale;
    if (@available(iOS 11.2, *)) gLayer.maximumDrawableCount = 3;
    [host addSubview:gView];
    gInFlight = dispatch_semaphore_create(kSlots);
    gWaitCtx = [[EAGLContext alloc] initWithAPI:ctx.API sharegroup:ctx.sharegroup];
    gPresentQ = dispatch_queue_create("hotd present",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    if (!gWaitCtx) {
        SDL_Log("ios: no second OpenGL ES context for the Metal present: OpenGL ES's (60 Hz)");
        return false;
    }
    SDL_Log("ios: frames shown by Metal (CAMetalLayer, up to %d Hz)", int(uiWindow.screen.maximumFramesPerSecond));
    return true;
}

} // namespace

namespace hotd::ios {

unsigned metalTarget(SDL_Window* window, int w, int h)
{
    if (!gTried) gOn = start(window);
    if (!gOn || w <= 0 || h <= 0) return 0;
    if (w != gW || h != gH) {
        for (int i = 0; i < kSlots; ++i) dispatch_semaphore_wait(gInFlight, DISPATCH_TIME_FOREVER);   // (none in use)
        freeSlots();
        const bool ok = makeSlots(w, h);
        for (int i = 0; i < kSlots; ++i) dispatch_semaphore_signal(gInFlight);
        if (!ok) {
            SDL_Log("ios: the Metal present's %d x %d surfaces could not be made: OpenGL ES's (60 Hz)", w, h);
            [gView removeFromSuperview];
            gOn = false;
            return 0;
        }
        SDL_Log("ios: Metal present: %d x %d", w, h);
    }
    dispatch_semaphore_wait(gInFlight, DISPATCH_TIME_FOREVER);   // (this slot: Metal done reading it)
    gCur = (gCur + 1) % kSlots;
    gPending = true;
    return gSlot[gCur].fbo;
}

bool metalFrameDrawn()
{
    if (!gPending) return false;
    gPending = false;
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();   // (the frame's commands to the GPU: the fence can signal)
    const int slot = gCur;
    const Uint64 t0 = SDL_GetTicksNS();
    dispatch_async(gPresentQ, ^{
        @autoreleasepool {
            if ([EAGLContext currentContext] != gWaitCtx) [EAGLContext setCurrentContext:gWaitCtx];
            if (fence) {
                glClientWaitSync(fence, 0, 200000000ull);   // (OpenGL ES done with the surface)
                glDeleteSync(fence);
            }
            gGpuSum = gGpuSum.load() + double(SDL_GetTicksNS() - t0) * 1e-6;
            ++gGpuN;
            id<CAMetalDrawable> d = [gLayer nextDrawable];
            id<MTLCommandBuffer> cmd = [gQueue commandBuffer];
            if (d && d.texture.width == size_t(gW) && d.texture.height == size_t(gH)) {
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                [blit copyFromTexture:CVMetalTextureGetTexture(gSlot[slot].mtlTex) sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(size_t(gW), size_t(gH), 1)
                            toTexture:d.texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                [cmd presentDrawable:d];
            }
            dispatch_semaphore_t sem = gInFlight;
            [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(sem); }];
            [cmd commit];
        }
    });
    return true;
}

double metalGpuMs()
{
    const int n = gGpuN.exchange(0);
    const double sum = gGpuSum.exchange(0);
    return n > 0 ? sum / n : -1;
}

} // namespace hotd::ios
