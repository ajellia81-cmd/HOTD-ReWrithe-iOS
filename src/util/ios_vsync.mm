// ios (2026-11-12, docs/ios.md "Frame pacing"): vsync for the iPhone / iPad. SDL's UIKit OpenGL ES has no swap interval
// ("Setting the swap interval is not supported"), so the game's pacing (bugs3: the swap waits for the display, the
// limiter only the fallback) fell back to the limiter alone: frames on the limiter's own clock, not the display's, so
// some refreshes showed a frame twice and the next one skipped (the first device test's "frame drops" while the game
// ran at its rate). Here a CADisplayLink on a thread of its own ticks with the display; SDL_GL_SetSwapInterval(n) is
// emulated (main.cpp, iOS only): the link asks for the display's rate / n, and every present waits for the next tick.
// Under LiveContainer the host app's Info.plist decides whether more than 60 a second are allowed: the link's real
// rate is measured and logged ("ios: display link N Hz").
#include "util/ios_app.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace {

std::mutex gMu;
std::condition_variable gCv;
uint64_t gTicks = 0;              // the display link's ticks so far
std::atomic<int> gInterval{0};    // the emulated swap interval (0: no wait)
std::atomic<bool> gRunning{false};
std::atomic<double> gMeasuredHz{0};
uint64_t gPresentedAt = 0;        // the tick the last frame waited for

} // namespace

@interface HotdVsync : NSObject
@property(nonatomic, strong) CADisplayLink* link;
@property(nonatomic) int maxFps;
@property(nonatomic) int wantFps;
@property(nonatomic) CFTimeInterval countStart;
@property(nonatomic) int counted;
@end

@implementation HotdVsync
- (void)tick:(CADisplayLink*)link
{
    {
        std::lock_guard<std::mutex> lock(gMu);
        ++gTicks;
    }
    gCv.notify_all();
    // the link's real rate over 2 s (LiveContainer: the host's Info.plist may hold it at 60)
    if (self.countStart == 0) self.countStart = link.timestamp;
    if (++self.counted >= 2 && link.timestamp - self.countStart >= 2.0) {
        const double hz = double(self.counted - 1) / (link.timestamp - self.countStart);
        const double was = gMeasuredHz.exchange(hz);
        if (std::abs(was - hz) > 3.0) SDL_Log("ios: display link %.0f Hz (asked %d)", hz, self.wantFps);
        self.countStart = link.timestamp, self.counted = 1;
    }
    // the rate asked for: the display's / the interval
    const int n = gInterval.load();
    const int want = n > 0 ? std::max(1, self.maxFps / n) : self.maxFps;
    if (want != self.wantFps) {
        self.wantFps = want;
        if (@available(iOS 15.0, *))
            link.preferredFrameRateRange = CAFrameRateRangeMake(float(want), float(want), float(want));
        else
            link.preferredFramesPerSecond = want;
        self.countStart = 0, self.counted = 0;
    }
}
- (void)run
{
    @autoreleasepool {
        self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
        const int n = gInterval.load();
        self.wantFps = n > 0 ? std::max(1, self.maxFps / n) : self.maxFps;
        if (@available(iOS 15.0, *))
            self.link.preferredFrameRateRange = CAFrameRateRangeMake(float(self.wantFps), float(self.wantFps), float(self.wantFps));
        else
            self.link.preferredFramesPerSecond = self.wantFps;
        [self.link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
        SDL_Log("ios: display link started (the screen up to %d Hz)", self.maxFps);
    }
    while (gRunning.load()) {
        @autoreleasepool {
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
        }
    }
    [self.link invalidate];
}
@end

namespace hotd::ios {

static HotdVsync* gVsync = nil;

bool setSwapInterval(int interval)
{
    gInterval = std::max(0, interval);
    if (interval > 0 && !gRunning.exchange(true)) {
        gVsync = [HotdVsync new];
        const int maxFps = int(UIScreen.mainScreen.maximumFramesPerSecond);   // (UIKit: here, on the main thread)
        gVsync.maxFps = maxFps > 0 ? maxFps : 60;
        NSThread* t = [[NSThread alloc] initWithTarget:gVsync selector:@selector(run) object:nil];
        t.name = @"hotd display link";
        t.qualityOfService = NSQualityOfServiceUserInteractive;
        [t start];
    }
    return true;   // (the game's pacing: the swap waits, beforePresent)
}

void waitForDisplay()
{
    const int n = gInterval.load();
    if (n <= 0 || !gRunning.load())
        return;
    // the next tick after the last frame's (a frame later than that: no wait, it goes out at once); a stalled link
    // (the app going to the background) never holds a frame longer than 100 ms
    std::unique_lock<std::mutex> lock(gMu);
    const uint64_t want = gPresentedAt + 1;
    gCv.wait_for(lock, std::chrono::milliseconds(100), [want] { return gTicks >= want; });
    gPresentedAt = gTicks;
}

double displayLinkHz()
{
    return gMeasuredHz.load();
}

std::string deviceState()
{
    // the phone's heat (iOS slows the CPU / GPU from "serious" on) and Low Power Mode (slower cores): the same place
    // ran at different rates in different runs
    static const char* const kHeat[4] = {"nominal", "fair", "serious", "critical"};
    const NSInteger t = NSProcessInfo.processInfo.thermalState;
    std::string s = std::string("heat ") + (t >= 0 && t < 4 ? kHeat[t] : "?");
    if (NSProcessInfo.processInfo.lowPowerModeEnabled) s += ", Low Power Mode";
    return s;
}

} // namespace hotd::ios
