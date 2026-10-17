// ============================================================================
//  PSBET — pacer.cpp (Phase 4) — frame pacing + vsync model.
// ============================================================================
#include "core/pacer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/log.h"
#include "core/timing.h"

namespace psb::av {

namespace {
u64 steadyUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sleepUsInterruptible(u64 us) {
    constexpr u64 kSlice = 1000;
    u64 remaining = us;
    while (remaining > 0) {
        const u64 step = remaining < kSlice ? remaining : kSlice;
        std::this_thread::sleep_for(std::chrono::microseconds(step));
        remaining -= step;
    }
}
} // namespace

void FramePacer::configure(u64 targetFps, bool vsync) {
    fps_ = targetFps;
    vsync_ = vsync && targetFps != 0;
    frames_ = 0;
    anchored_ = false;
    lastPresentUs_ = 0;
    intervalCount_ = 0;
    intervalPos_ = 0;
    inFlight_.store(0);
    vsyncWaits_.store(0);
    if (fps_)
        LOG_INFO("pacer: target FPS authorized: %llu (vsync %s)",
                 static_cast<unsigned long long>(fps_), vsync_ ? "on" : "off");
}

void FramePacer::onFrameSubmitted() {
    if (!vsync_) return;
    // Present-queue backpressure: never more than depth frames in flight.
    while (inFlight_.fetch_add(1, std::memory_order_acq_rel) >= kPresentQueueDepth) {
        inFlight_.fetch_sub(1, std::memory_order_release);
        vsyncWaits_.fetch_add(1, std::memory_order_relaxed);
        sleepUsInterruptible(500);
    }
}

void FramePacer::onFramePresented() {
    const u64 now = steadyUs();

    // Interval bookkeeping (free-run mode too — stats are free).
    if (lastPresentUs_ != 0) {
        const u32 interval = static_cast<u32>(now - lastPresentUs_);
        intervals_[intervalPos_] = interval;
        intervalPos_ = (intervalPos_ + 1) % kIntervalRing;
        if (intervalCount_ < kIntervalRing) ++intervalCount_;
    }
    lastPresentUs_ = now;

    if (inFlight_.load(std::memory_order_acquire) > 0)
        inFlight_.fetch_sub(1, std::memory_order_release);

    if (fps_ == 0) {
        ++frames_;
        return;
    }

    // Absolute schedule: frame N presents at start + N * period.
    if (!anchored_) {
        startUs_ = now;
        anchored_ = true;
    }
    const u64 period = 1'000'000ull / fps_;
    const u64 slot = startUs_ + frames_ * period;
    ++frames_;
    if (now < slot) sleepUsInterruptible(slot - now);
}

FramePacer::Stats FramePacer::stats() const {
    Stats s;
    s.frames = frames_;
    s.authorizedFps = fps_;
    s.vsync = vsync_;
    s.vsyncWaits = vsyncWaits_.load(std::memory_order_relaxed);
    if (intervalCount_ == 0) return s;

    u32 sorted[kIntervalRing];
    std::memcpy(sorted, intervals_, sizeof(sorted));
    std::sort(sorted, sorted + intervalCount_);

    u64 sum = 0;
    for (u32 i = 0; i < intervalCount_; ++i) sum += sorted[i];
    s.minIntervalMs = static_cast<double>(sorted[0]) / 1000.0;
    s.maxIntervalMs = static_cast<double>(sorted[intervalCount_ - 1]) / 1000.0;
    s.meanIntervalMs = static_cast<double>(sum) / intervalCount_ / 1000.0;
    const u32 p99idx = static_cast<u32>(intervalCount_ * 99 / 100);
    s.p99IntervalMs = static_cast<double>(sorted[p99idx]) / 1000.0;
    return s;
}

} // namespace psb::av

#ifdef PSBET_WITH_SDL3

#include <SDL3/SDL.h>

namespace psb::av {

struct SdlPresenter::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    u32 width = 0, height = 0;
};

SdlPresenter::SdlPresenter(u32 width, u32 height) {
    impl_ = new Impl;
    impl_->width = width;
    impl_->height = height;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_WARN("present-sdl: SDL_Init(video) failed: %s", SDL_GetError());
        delete impl_;
        impl_ = nullptr;
        return;
    }
    impl_->window = SDL_CreateWindow("PSBET", static_cast<int>(width),
                                     static_cast<int>(height), 0);
    if (!impl_->window) {
        LOG_WARN("present-sdl: window creation failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        delete impl_;
        impl_ = nullptr;
        return;
    }
    impl_->renderer = SDL_CreateRenderer(impl_->window, nullptr);
    if (!impl_->renderer) {
        LOG_WARN("present-sdl: renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(impl_->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        delete impl_;
        impl_ = nullptr;
        return;
    }
    impl_->texture = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       static_cast<int>(width),
                                       static_cast<int>(height));
    if (!impl_->texture) {
        LOG_WARN("present-sdl: texture creation failed: %s", SDL_GetError());
        SDL_DestroyRenderer(impl_->renderer);
        SDL_DestroyWindow(impl_->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        delete impl_;
        impl_ = nullptr;
        return;
    }
    valid_ = true;
    LOG_INFO("present-sdl: window %ux%u online (driver: %s)", width, height,
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
}

SdlPresenter::~SdlPresenter() {
    if (!impl_) return;
    if (impl_->texture) SDL_DestroyTexture(impl_->texture);
    if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window) SDL_DestroyWindow(impl_->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    delete impl_;
    impl_ = nullptr;
}

bool SdlPresenter::present(const u8* rgba, u32 width, u32 height) {
    if (!impl_ || !valid_) return false;
    if (width != impl_->width || height != impl_->height) return false;
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(impl_->texture, nullptr, &pixels, &pitch)) return false;
    std::memcpy(pixels, rgba, static_cast<size_t>(width) * height * 4);
    SDL_UnlockTexture(impl_->texture);
    SDL_RenderClear(impl_->renderer);
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr);
    SDL_RenderPresent(impl_->renderer);
    // Keep the window responsive on interactive drivers.
    SDL_PumpEvents();
    return true;
}

} // namespace psb::av

#endif // PSBET_WITH_SDL3
