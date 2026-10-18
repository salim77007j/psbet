// ============================================================================
//  PSBET — emulator.h
//  Top-level orchestrator: memory -> loader -> shims -> CPU supervisor -> GPU.
// ============================================================================
#pragma once

#include <string>
#include "psbet/common.h"

namespace psb {

struct EmuOptions {
    u32         width = 1280;
    u32         height = 720;
    std::string screenshotDir;       // empty = no capture
    u32         screenshotInterval = 30;   // capture every N frames
    u32         maxFrames = 1000000;       // safety frame limit
    bool        verbose = false;
    std::string backend = "auto";    // auto | software | vulkan
    std::string mountImage;          // read-only app0:/ container (Phase 3)
    std::string dataDir;             // writable /data host dir (Phase 3)
    std::string syscallTrap = "on";  // on | off | required (Phase 3)
    // Phase 4 — audio, input, timing:
    u64         targetFps = 0;       // 0 = free run; 60/120 = authorized pacing
    bool        vsync = false;       // present-queue backpressure (with fps)
    std::string audioDump;           // WAV path -> mixed-master dump backend
    std::string padScript;           // scripted pad events (time-keyed)
    std::string window = "off";      // on -> SDL3 presentation window (opt.)
    // Phase 5 — performance:
    bool        gpuSync = false;     // --gpu-sync: disable pipelined submits
    u32         gpuThreads = 0;      // --gpu-threads N: raster band workers (0=auto)
};

struct EmuStats {
    i32    guestExitCode = 0;
    bool   exitedViaGuest = false;
    bool   crashed = false;
    u64    frames = 0;
    u64    elapsedMs = 0;
    double avgFps = 0.0;
    u32    screenshots = 0;
    u64    peakRssKiB = 0;
    std::string crashReport;
    std::string backendUsed;         // "software" | "vulkan"
    // Phase-3 fidelity accounting:
    u32    threadsJoined = 0;        // guest threads joined at teardown
    u32    threadsDetached = 0;      // force-detached stragglers (0 = clean)
    u32    threadsParked = 0;        // Windows recorded-exit parks
    u64    memoryUsedBytes = 0;      // physical bytes live at exit
    // Phase-4 audio/input/timing accounting:
    std::string audioBackendUsed;    // wav-dump | sdl3 | null | none
    u64    audioSubmittedFrames = 0; // master frames submitted by the guest
    u64    audioMixedFrames = 0;     // master frames consumed by the backend
    u64    audioDroppedFrames = 0;   // INVARIANT: 0
    u32    audioMaxLatencyMs = 0;    // max queue depth observed at submit
    u32    audioBackpressureWaits = 0;
    double frameMeanIntervalMs = 0.0;
    double frameMaxIntervalMs = 0.0;
    u32    vsyncWaits = 0;
    u64    netStubCalls = 0;
    std::string presenterUsed;       // offscreen | sdl3-window
    // Phase-5 GPU queue / raster accounting:
    bool   gpuPipelined = false;     // submit path mode for this run
    u64    gpuEnqueued = 0;          // graphics CBs accepted by the queue
    u64    gpuCompleted = 0;         // graphics CBs executed by the worker
    u64    gpuBackpressureWaits = 0; // guest submits blocked by the cap
    u32    gpuMaxInFlight = 0;       // deepest observed work-ahead
    u32    gpuMaxInFlightCap = 0;    // configured bound
    u64    gpuExecuteUs = 0;         // worker execution time (graphics)
    u64    gpuComputeSubmits = 0;    // compute-queue CBs
    u32    rasterThreads = 0;        // band workers used by the software backend
    bool   rasterSimd = false;       // AVX fast path active
};

class Emulator {
public:
    // Runs a guest ELF end-to-end. Never throws; all failures are encoded in
    // the returned stats (and logged).
    EmuStats run(const std::string& elfPath, const EmuOptions& options);
};

} // namespace psb
