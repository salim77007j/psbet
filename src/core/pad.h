// ============================================================================
//  PSBET — pad.h (Phase 4)
//  Guest input service: DualSense-style pad state exposed through an Orbis-
//  shaped scePad* surface (GUEST_ABI §10). State comes from a pluggable
//  driver:
//
//    * ScriptedPadDriver — deterministic time-keyed event script (CI/tests,
//      and a reproducible input-replay path for bug reports)
//    * NullPadDriver     — every pad disconnected
//    * SdlGamepadDriver  — SDL3 game controllers + gyro sensors (optional
//      build; real DualSense mapping lives here)
//
//  Latency contract: every scripted state carries the guest-domain time it
//  became due (PadState.timestampUs); a guest polling at t observes
//  end-to-end latency = t - timestampUs. At 60 FPS pacing this is bounded by
//  one frame interval + poll offset — the < 25 ms acceptance is measured by
//  the input_demo guest itself and asserted by the test suite.
// ============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "psbet/common.h"

namespace psb::pad {

constexpr u32 kMaxHandles = 4;

// Button bits (PSBET definition, GUEST_ABI §10) — DualSense placement.
enum PadButtons : u32 {
    PadBtnCross     = 1u << 0,
    PadBtnCircle    = 1u << 1,
    PadBtnSquare    = 1u << 2,
    PadBtnTriangle  = 1u << 3,
    PadBtnL1        = 1u << 4,
    PadBtnR1        = 1u << 5,
    PadBtnL2        = 1u << 6,   // digital part of the analog trigger
    PadBtnR2        = 1u << 7,
    PadBtnOptions   = 1u << 8,
    PadBtnShare     = 1u << 9,
    PadBtnPs        = 1u << 10,
    PadBtnTouchpad  = 1u << 11,
    PadBtnUp        = 1u << 12,
    PadBtnDown      = 1u << 13,
    PadBtnLeft      = 1u << 14,
    PadBtnRight     = 1u << 15,
    PadBtnL3        = 1u << 16,
    PadBtnR3        = 1u << 17,
};

// Connection types (PSBET definition).
enum ConnectionType : u32 {
    ConnNone       = 0,
    ConnUsb        = 1,
    ConnBluetooth  = 2,
};

struct PadState {
    u32     buttons = 0;
    u8      lx = 128, ly = 128, rx = 128, ry = 128;   // sticks, 0..255
    u8      l2 = 0, r2 = 0;                           // analog triggers
    float   accelX = 0, accelY = 0, accelZ = 0;       // g
    float   gyroX = 0, gyroY = 0, gyroZ = 0;          // deg/s
    u32     connected = 0;
    u64     timestampUs = 0;   // guest-domain time this state became valid
};

struct ControllerInfo {
    u32 touchpixW = 1920, touchpixH = 1080;   // touchpad pixel resolution
    u32 touchresX = 1920, touchresY = 940;    // touchpad sampling resolution
    u32 connectionType = ConnNone;
    u32 connectedCount = 0;
};

// ---------------------------------------------------------------------------
// IPadDriver — produces the per-pad snapshot the service serves to guests.
// ---------------------------------------------------------------------------
struct PadSnapshot {
    PadState state{};
    u64 dueUs = 0;   // when this snapshot became valid (latency bookkeeping)
};

class IPadDriver {
public:
    virtual ~IPadDriver() = default;
    // Refreshes snapshots[0..kMaxHandles). Called once per engine frame.
    virtual void poll(PadSnapshot* snapshots, u64 nowUs) = 0;
    virtual const char* name() const = 0;
};

class NullPadDriver final : public IPadDriver {
public:
    void poll(PadSnapshot*, u64) override {}
    const char* name() const override { return "null"; }
};

// Time-keyed deterministic events. Script format (one event per line,
// '#' comments allowed):
//   <time_ms> <buttons_hex|0> <lx> <ly> <rx> <ry> <l2> <r2>
//   <time_ms> GYRO <ax> <ay> <az> <gx> <gy> <gz>
// Each event holds until the next event's time. Sticks/triggers are absolute
// values; buttons are an absolute bitmask (transition detection is the
// guest's job, like a real controller).
class ScriptedPadDriver final : public IPadDriver {
public:
    // Loads a script; returns false (and logs) on a malformed line.
    bool loadFromFile(const char* path);
    bool loadFromString(const char* spec);
    void poll(PadSnapshot* snapshots, u64 nowUs) override;
    const char* name() const override { return "scripted"; }
    u32 eventCount() const { return static_cast<u32>(events_.size()); }

private:
    struct Event {
        u64  atUs = 0;
        u32  buttons = 0;
        u8   lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
        float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
        bool motion = false;
    };
    bool parseLine(const std::string& line);
    std::vector<Event> events_;   // sorted by time
};

#ifdef PSBET_WITH_SDL3
// SDL3 game controller + sensor driver (real DualSense mapping). Pads that
// exist map to handles in open order; absent pads report disconnected.
class SdlGamepadDriver final : public IPadDriver {
public:
    SdlGamepadDriver();
    ~SdlGamepadDriver() override;
    void poll(PadSnapshot* snapshots, u64 nowUs) override;
    const char* name() const override { return "sdl3-gamepad"; }
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
#endif

// ---------------------------------------------------------------------------
// PadService — handle table + guest surface plumbing.
// ---------------------------------------------------------------------------
class PadService {
public:
    static PadService& get();

    void setDriver(std::unique_ptr<IPadDriver> driver);
    void clearDriver();
    // Engine per-frame refresh (keeps the driver + latency bookkeeping live).
    void poll();

    // Guest surface (GUEST_ABI §10). handle is 1-based; -1 on failure.
    i32 open(u32 userId, i32 type, i32 index, const void* param);
    i32 readState(i32 handle, PadState* out);
    // Batch read: fills up to `count` historical entries; PSBET serves the
    // single current state and reports how many entries were filled (1 when
    // connected, 0 when not).
    i32 read(i32 handle, PadState* out, u32 count, u32* returned);
    i32 getControllerInformation(i32 handle, ControllerInfo* out);

    struct Stats {
        u64 driverPolls = 0;
        u64 maxLatencyUs = 0;   // max due->service-visibility delay observed
    };
    Stats stats() const;

private:
    PadService() = default;
    void recordLatency(const PadSnapshot& snap, u64 nowUs);
    // Shared driver refresh under mtx_ — called from the engine frame path
    // and from the guest read paths (freshness independent of GPU cadence).
    void refreshLocked(u64 now);

    mutable std::mutex mtx_;
    std::unique_ptr<IPadDriver> driver_;
    PadSnapshot snaps_[kMaxHandles]{};
    bool handleUsed_[kMaxHandles]{};
    u64 lastDueUs_[kMaxHandles]{};
    u64 driverPolls_ = 0;
    u64 maxLatencyUs_ = 0;
};

// Guest-visible pad surface: declared with C linkage in the standalone
// guest header (include/psbet/psb_guest_api.h, GUEST_ABI §10) and defined in
// pad.cpp against those ABI structs (layout-identical to the engine types
// above). The kernel export table resolves the names globally.

} // namespace psb::pad
