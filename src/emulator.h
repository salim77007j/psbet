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
};

class Emulator {
public:
    // Runs a guest ELF end-to-end. Never throws; all failures are encoded in
    // the returned stats (and logged).
    EmuStats run(const std::string& elfPath, const EmuOptions& options);
};

} // namespace psb
