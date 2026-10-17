// ============================================================================
//  PSBET — pad.cpp (Phase 4) — pad service + scripted driver.
// ============================================================================
#include "core/pad.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "core/log.h"
#include "core/timing.h"
#include "psbet/psb_guest_api.h"

namespace psb::pad {

// ---- ScriptedPadDriver -------------------------------------------------------
bool ScriptedPadDriver::loadFromFile(const char* path) {
    std::ifstream in(path);
    if (!in) {
        LOG_WARN("pad: script open failed: %s", path);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) parseLine(line);
    return !events_.empty();
}

bool ScriptedPadDriver::loadFromString(const char* spec) {
    std::string s(spec ? spec : "");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        parseLine(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return !events_.empty();
}

bool ScriptedPadDriver::parseLine(const std::string& line) {
    // Strip comments/whitespace-only lines.
    const size_t hash = line.find('#');
    std::string body = line.substr(0, hash);
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) return true;

    std::istringstream ss(body);
    unsigned long long tms = 0;
    if (!(ss >> tms)) {
        LOG_WARN("pad: script line ignored (no time): %s", body.c_str());
        return true;
    }
    Event ev;
    ev.atUs = tms * 1000ull;

    std::string tok;
    if (!(ss >> tok)) {
        LOG_WARN("pad: script line ignored (no payload): %s", body.c_str());
        return true;
    }
    if (tok == "GYRO") {
        ev.motion = true;
        if (!(ss >> ev.ax >> ev.ay >> ev.az >> ev.gx >> ev.gy >> ev.gz)) {
            LOG_WARN("pad: GYRO line malformed: %s", body.c_str());
            return true;
        }
    } else {
        unsigned buttons = 0;
        try {
            buttons = static_cast<unsigned>(std::stoul(tok, nullptr, 16));
        } catch (...) {
            LOG_WARN("pad: button mask malformed: %s", body.c_str());
            return true;
        }
        ev.buttons = buttons;
        int lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
        ss >> lx >> ly >> rx >> ry >> l2 >> r2;   // optional tail
        ev.lx = static_cast<u8>(lx < 0 ? 0 : (lx > 255 ? 255 : lx));
        ev.ly = static_cast<u8>(ly < 0 ? 0 : (ly > 255 ? 255 : ly));
        ev.rx = static_cast<u8>(rx < 0 ? 0 : (rx > 255 ? 255 : rx));
        ev.ry = static_cast<u8>(ry < 0 ? 0 : (ry > 255 ? 255 : ry));
        ev.l2 = static_cast<u8>(l2 < 0 ? 0 : (l2 > 255 ? 255 : l2));
        ev.r2 = static_cast<u8>(r2 < 0 ? 0 : (r2 > 255 ? 255 : r2));
    }
    events_.push_back(ev);
    std::stable_sort(events_.begin(), events_.end(),
                     [](const Event& a, const Event& b) { return a.atUs < b.atUs; });
    return true;
}

void ScriptedPadDriver::poll(PadSnapshot* snapshots, u64 nowUs) {
    if (events_.empty()) return;
    // Find the last event at/before nowUs — that IS the current state.
    const Event* cur = nullptr;
    for (const Event& e : events_) {
        if (e.atUs > nowUs) break;
        cur = &e;
    }
    PadSnapshot& s = snapshots[0];   // scripted driver drives pad handle 1
    if (!cur) {
        s = PadSnapshot{};           // pre-first-event: neutral, disconnected
        return;
    }
    s.state.buttons = cur->buttons;
    s.state.lx = cur->lx;  s.state.ly = cur->ly;
    s.state.rx = cur->rx;  s.state.ry = cur->ry;
    s.state.l2 = cur->l2;  s.state.r2 = cur->r2;
    if (cur->motion) {
        s.state.accelX = cur->ax; s.state.accelY = cur->ay; s.state.accelZ = cur->az;
        s.state.gyroX = cur->gx; s.state.gyroY = cur->gy; s.state.gyroZ = cur->gz;
    }
    s.state.connected = 1;
    s.state.timestampUs = cur->atUs;
    s.dueUs = cur->atUs;
}

// ---- PadService ---------------------------------------------------------------
PadService& PadService::get() {
    static PadService inst;
    return inst;
}

void PadService::setDriver(std::unique_ptr<IPadDriver> driver) {
    std::lock_guard<std::mutex> lk(mtx_);
    driver_ = std::move(driver);
    driverPolls_ = 0;
    maxLatencyUs_ = 0;
    for (u32 i = 0; i < kMaxHandles; ++i) {
        snaps_[i] = PadSnapshot{};
        handleUsed_[i] = false;
        lastDueUs_[i] = 0;
    }
}

void PadService::clearDriver() {
    std::lock_guard<std::mutex> lk(mtx_);
    driver_.reset();
}

void PadService::recordLatency(const PadSnapshot& snap, u64 nowUs) {
    if (snap.dueUs > 0 && snap.dueUs <= nowUs && nowUs - snap.dueUs > maxLatencyUs_)
        maxLatencyUs_ = nowUs - snap.dueUs;
}

// Shared refresh under mtx_. Called from the engine frame path (GPU submit)
// AND from the guest read paths — input freshness must NOT depend on GPU
// render time (a slow backend must not delay when guests observe input;
// real hardware samples input independent of the GPU pipeline).
void PadService::refreshLocked(u64 now) {
    ++driverPolls_;
    if (!driver_) return;
    driver_->poll(snaps_, now);
    for (u32 i = 0; i < kMaxHandles; ++i)
        if (handleUsed_[i]) recordLatency(snaps_[i], now);
}

void PadService::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    refreshLocked(time::TimeDomain::get().us());
}

i32 PadService::open(u32 userId, i32 type, i32 index, const void* param) {
    (void)param;
    if (userId != 0) {
        LOG_WARN("pad: userId %u unsupported (PSBET single-user, use 0)", userId);
        return -1;
    }
    if (type != 0) {
        LOG_WARN("pad: pad type %d unsupported (0 = standard)", type);
        return -1;
    }
    if (index != 0) {
        LOG_WARN("pad: pad index %d unsupported (0 = first pad)", index);
        return -1;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!handleUsed_[0]) {
        handleUsed_[0] = true;
        LOG_DEBUG("pad: handle 1 open (driver=%s)",
                  driver_ ? driver_->name() : "none");
        return 1;
    }
    for (u32 i = 1; i < kMaxHandles; ++i) {
        if (handleUsed_[i]) continue;
        handleUsed_[i] = true;
        return static_cast<i32>(i) + 1;
    }
    LOG_WARN("pad: no free handles (%u max)", kMaxHandles);
    return -1;
}

i32 PadService::readState(i32 handle, PadState* out) {
    if (handle < 1 || handle > static_cast<i32>(kMaxHandles) || !out) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!handleUsed_[handle - 1]) return -1;
    // On-demand refresh: the served state is sampled NOW, not at the last
    // GPU submit — decouples input observation latency from render time.
    refreshLocked(time::TimeDomain::get().us());
    *out = snaps_[handle - 1].state;
    return 0;
}

i32 PadService::read(i32 handle, PadState* out, u32 count, u32* returned) {
    if (handle < 1 || handle > static_cast<i32>(kMaxHandles) || !out || count == 0)
        return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!handleUsed_[handle - 1]) return -1;
    refreshLocked(time::TimeDomain::get().us());   // same freshness contract
    if (returned) *returned = snaps_[handle - 1].state.connected ? 1u : 0u;
    if (snaps_[handle - 1].state.connected) out[0] = snaps_[handle - 1].state;
    return 0;
}

i32 PadService::getControllerInformation(i32 handle, ControllerInfo* out) {
    if (handle < 1 || handle > static_cast<i32>(kMaxHandles) || !out) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!handleUsed_[handle - 1]) return -1;
    *out = ControllerInfo{};
    out->connectedCount = snaps_[handle - 1].state.connected ? 1u : 0u;
    return 0;
}

PadService::Stats PadService::stats() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stats s;
    s.driverPolls = driverPolls_;
    s.maxLatencyUs = maxLatencyUs_;
    return s;
}

// ---------------------------------------------------------------------------
// Guest-visible pad surface (C linkage, declared in psb_guest_api.h).
// ---------------------------------------------------------------------------
extern "C" {

int32_t scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param) {
    return pad::PadService::get().open(static_cast<u32>(userId), type, index,
                                       param);
}

int32_t scePadReadState(int32_t handle, PsbPadState* out) {
    return pad::PadService::get().readState(handle,
        reinterpret_cast<pad::PadState*>(out));
}

int32_t scePadRead(int32_t handle, PsbPadState* out, uint32_t count,
                   uint32_t* returned) {
    return pad::PadService::get().read(handle,
        reinterpret_cast<pad::PadState*>(out), count, returned);
}

int32_t scePadGetControllerInformation(int32_t handle,
                                       PsbPadControllerInfo* out) {
    return pad::PadService::get().getControllerInformation(handle,
        reinterpret_cast<pad::ControllerInfo*>(out));
}

} // extern "C"

#if defined(_WIN32)
// Windows SysV->Win64 trampoline targets (w64_thunks.asm).
extern "C" {
int32_t psbXPadOpen(int32_t a, int32_t b, int32_t c, const void* d)
    { return scePadOpen(a, b, c, d); }
int32_t psbXPadReadState(int32_t a, PsbPadState* b)
    { return scePadReadState(a, b); }
int32_t psbXPadRead(int32_t a, PsbPadState* b, uint32_t c, uint32_t* d)
    { return scePadRead(a, b, c, d); }
int32_t psbXPadGetControllerInformation(int32_t a, PsbPadControllerInfo* b)
    { return scePadGetControllerInformation(a, b); }
}
#endif

} // namespace psb::pad
