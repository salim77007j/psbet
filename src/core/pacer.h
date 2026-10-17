// ============================================================================
//  PSBET — pacer.h (Phase 4)
//  Frame pacing + vsync model + presentation abstraction.
//
//  Target-FPS authorization (console boost-mode analogue, GUEST_ABI §7):
//  the engine authorizes a maximum presentation rate; each presented frame
//  is scheduled on an ABSOLUTE timeline (frame N presents at start + N/fps)
//  — not by sleeping a relative interval — so pacing error can never
//  accumulate (the same drift-free rule as the guest clock).
//
//  Vsync model: the present queue holds at most kPresentQueueDepth frames.
//  A submit beyond the depth blocks (guest-cooperative) until a slot frees.
//  This is the "presentation backpressure" a real vsync'd swapchain applies
//  and is what keeps a guest that submits faster than the authorized rate
//  pinned to it.
// ============================================================================
#pragma once

#include <atomic>
#include "psbet/common.h"

namespace psb::av {

constexpr u32 kPresentQueueDepth = 2;

class FramePacer {
public:
    // targetFps 0 = free run (soak mode). vsync applies the present-queue
    // backpressure (only meaningful with targetFps > 0).
    void configure(u64 targetFps, bool vsync);

    // Called by the GPU device when a guest submit completes (a frame was
    // produced). vsync: blocks while the present queue is over depth.
    void onFrameSubmitted();

    // Called after the presenter accepted the frame: sleeps until the
    // frame's absolute slot, then returns. Free-run mode: no-op.
    void onFramePresented();

    struct Stats {
        u64  frames = 0;
        double meanIntervalMs = 0.0;
        double minIntervalMs = 0.0;
        double maxIntervalMs = 0.0;
        double p99IntervalMs = 0.0;
        u32  vsyncWaits = 0;
        u64  authorizedFps = 0;
        bool vsync = false;
    };
    Stats stats() const;

private:
    u64  fps_ = 0;
    bool vsync_ = false;
    u64  frames_ = 0;
    u64  startUs_ = 0;          // first paced frame's host anchor
    bool anchored_ = false;

    // Interval tracking (bounded ring of the last 256 intervals, us).
    static constexpr u32 kIntervalRing = 256;
    u32 intervals_[kIntervalRing] = {};
    u32 intervalCount_ = 0;
    u32 intervalPos_ = 0;
    u64 lastPresentUs_ = 0;

    std::atomic<u32> vsyncWaits_{0};
    std::atomic<u64> inFlight_{0};   // submitted-but-not-presented
};

// ---------------------------------------------------------------------------
// Presentation — offscreen by default; SDL3 window when built + requested.
// ---------------------------------------------------------------------------
class IPresenter {
public:
    virtual ~IPresenter() = default;
    virtual bool present(const u8* rgba, u32 width, u32 height) = 0;
    virtual const char* name() const = 0;
};

// Offscreen presentation (headless/CI): the framebuffer stays memory-only;
// PNG capture happens separately via the frame hook.
class OffscreenPresenter final : public IPresenter {
public:
    bool present(const u8*, u32, u32) override { return true; }
    const char* name() const override { return "offscreen"; }
};

#ifdef PSBET_WITH_SDL3
// SDL3 window + streaming texture (real display path; works under the CI
// dummy video driver too, which makes the window path CI-verifiable).
class SdlPresenter final : public IPresenter {
public:
    explicit SdlPresenter(u32 width, u32 height);
    ~SdlPresenter() override;
    bool present(const u8* rgba, u32 width, u32 height) override;
    const char* name() const override { return "sdl3-window"; }
    bool valid() const { return valid_; }
private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool valid_ = false;
};
#endif

} // namespace psb::av
