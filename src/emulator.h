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
};

class Emulator {
public:
    // Runs a guest ELF end-to-end. Never throws; all failures are encoded in
    // the returned stats (and logged).
    EmuStats run(const std::string& elfPath, const EmuOptions& options);
};

} // namespace psb
