// ============================================================================
//  PSBET — audio.cpp (Phase 4) — AudioOut device model, mixer, WAV backend.
// ============================================================================
#include "core/audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/timing.h"

namespace psb::audio {

// ---- NullBackend -----------------------------------------------------------
bool NullBackend::start(u32, u32) { return true; }
void NullBackend::stop() {}
void NullBackend::feed(const i16*, u32 frames) { consumed_ += frames; }

// ---- WavDumpBackend ----------------------------------------------------------
bool WavDumpBackend::start(u32 rate, u32 channels) {
    rate_ = rate;
    channels_ = channels;
    samples_.clear();
    written_ = false;
    return true;
}

void WavDumpBackend::feed(const i16* interleaved, u32 frames) {
    samples_.insert(samples_.end(), interleaved, interleaved + frames * channels_);
}

void WavDumpBackend::stop() {
    if (written_ || samples_.empty() || path_.empty()) return;
    if (writeWavFile(path_.c_str(), rate_, channels_, samples_.data(),
                     samples_.size())) {
        LOG_INFO("audio: WAV dump written: %s (%llu frames, %u Hz, %u ch)",
                 path_.c_str(),
                 static_cast<unsigned long long>(samples_.size() / channels_),
                 rate_, channels_);
    } else {
        LOG_WARN("audio: WAV dump write FAILED: %s", path_.c_str());
    }
    written_ = true;
}

// ---- WAV writer (canonical 44-byte header, PCM16) ----------------------------
bool writeWavFile(const char* path, u32 rate, u32 channels,
                  const i16* samples, u64 sampleCount) {
    if (!path || !samples || sampleCount == 0 || channels == 0 || rate == 0)
        return false;
    const u64 dataBytes = sampleCount * sizeof(i16);
    if (dataBytes > 0xFFFFFFF0ull) return false;   // WAV32 limit
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    auto wu32 = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto wu16 = [&](u16 v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    wu32(36u + static_cast<u32>(dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    wu32(16); wu16(1 /*PCM*/);
    wu16(static_cast<u16>(channels));
    wu32(rate);
    wu32(rate * channels * sizeof(i16));           // byte rate
    wu16(static_cast<u16>(channels * sizeof(i16)));// block align
    wu16(16);                                       // bits per sample
    std::fwrite("data", 1, 4, f);
    wu32(static_cast<u32>(dataBytes));
    std::fwrite(samples, 1, static_cast<size_t>(dataBytes), f);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

// ---- Mixer -------------------------------------------------------------------
Mixer& Mixer::get() {
    static Mixer inst;
    return inst;
}

void Mixer::setBackend(std::unique_ptr<IAudioBackend> backend) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (backend_) backend_->stop();
    backend_ = std::move(backend);
    ring_.assign(kRingFrames * 2, 0);
    scratch_.clear();
    masterRead_ = frontier_ = submitted_ = mixed_ = dropped_ = underrun_ = 0;
    maxLatencyMs_ = 0;
    backpressureWaits_ = 0;
    pacingActive_ = false;
    if (backend_) {
        if (!backend_->start(kMasterRate, kMasterChannels)) {
            LOG_WARN("audio: backend '%s' failed to start — audio disabled",
                     backend_->name());
            backend_.reset();
        } else {
            LOG_INFO("audio: backend '%s' active (%u Hz, %u ch)",
                     backend_->name(), kMasterRate, kMasterChannels);
        }
    }
}

void Mixer::clearBackend() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (backend_) backend_->stop();
    backend_.reset();
    ring_.clear();
    scratch_.clear();
    for (Port& p : ports_) p = Port{};
    masterRead_ = frontier_ = submitted_ = mixed_ = dropped_ = underrun_ = 0;
    maxLatencyMs_ = 0;
    backpressureWaits_ = 0;
    pacingActive_ = false;
}

i32 Mixer::open(PortType type, u32 index, u32 lenFrames, u32 freq, Format fmt) {
    if (index != 0) {
        LOG_WARN("audio: port index %u unsupported (PSBET exposes index 0)", index);
        return -1;
    }
    if (freq != kMasterRate) {
        LOG_WARN("audio: port rate %u rejected — master domain is fixed at "
                 "%u Hz (GUEST_ABI §9)", freq, kMasterRate);
        return -1;
    }
    if (fmt > FmtFloatStereo) {
        LOG_WARN("audio: format %u invalid", fmt);
        return -1;
    }
    if (lenFrames == 0 || lenFrames > kMaxSubmitFrames) {
        LOG_WARN("audio: lenFrames %u out of range", lenFrames);
        return -1;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    for (u32 i = 0; i < kMaxPorts; ++i) {
        Port& p = ports_[i];
        if (p.used) continue;
        p.used = true;
        p.type = type;
        p.fmt = fmt;
        p.lenFrames = lenFrames;
        p.freq = freq;
        p.volL = p.volR = kVolMax;
        // A new port joins the timeline at the current frontier (never in
        // the past): its first submit plays with whatever is already queued.
        p.cursor = frontier_;
        LOG_DEBUG("audio: port %u open (type=%u len=%u fmt=%u)", i, type,
                  lenFrames, fmt);
        return static_cast<i32>(i) + 1;   // 1-based handle
    }
    LOG_WARN("audio: no free port slots (%u max)", kMaxPorts);
    return -1;
}

i32 Mixer::setVolume(i32 handle, u32 chBitmask, const i32* volumes) {
    if (handle < 1 || handle > static_cast<i32>(kMaxPorts) || !volumes) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    Port& p = ports_[handle - 1];
    if (!p.used) return -1;
    if (chBitmask & 0x1u)
        p.volL = volumes[0] < 0 ? 0
               : (volumes[0] > static_cast<i32>(kVolMax) ? static_cast<i32>(kVolMax)
                                                          : volumes[0]);
    if (chBitmask & 0x2u)
        p.volR = volumes[1] < 0 ? 0
               : (volumes[1] > static_cast<i32>(kVolMax) ? static_cast<i32>(kVolMax)
                                                          : volumes[1]);
    return 0;
}

i32 Mixer::close(i32 handle) {
    if (handle < 1 || handle > static_cast<i32>(kMaxPorts)) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!ports_[handle - 1].used) return -1;
    ports_[handle - 1] = Port{};
    return 0;
}

u64 Mixer::submitToMaster(Port& p, const void* ptr) {
    // Convert + volume-apply + SUM into the master ring at the port cursor.
    const auto* s16p = static_cast<const i16*>(ptr);
    const auto* f32p = static_cast<const float*>(ptr);
    const i32 vl = p.volL, vr = p.volR;
    // SIGNED constant for the volume division: dividing an i32 product by a
    // u32 constant promotes the product to unsigned (negative samples become
    // huge positives — found by the tone test, never shipped).
    const i32 volMax = static_cast<i32>(kVolMax);
    const u64 n = kRingFrames;

    u64 late = 0;
    for (u32 i = 0; i < p.lenFrames; ++i) {
        i32 l = 0, r = 0;
        switch (p.fmt) {
        case FmtS16Mono:
            l = r = s16p[i];
            break;
        case FmtS16Stereo:
            l = s16p[2 * i]; r = s16p[2 * i + 1];
            break;
        case FmtFloatStereo:
            l = static_cast<i32>(f32p[2 * i] * 32767.0f);
            r = static_cast<i32>(f32p[2 * i + 1] * 32767.0f);
            break;
        }
        const u64 frame = p.cursor + i;
        if (frame < masterRead_) { ++late; continue; }   // LATE: already played
        const u64 idx = (frame % n) * 2;
        ring_[idx]     += (l * vl) / volMax;
        ring_[idx + 1] += (r * vr) / volMax;
    }
    p.cursor += p.lenFrames;
    if (p.cursor > frontier_) frontier_ = p.cursor;
    return late;
}

bool Mixer::waitForRoom() {
    u64 spins = 0;
    while (frontier_ > masterRead_ &&
           frontier_ - masterRead_ >= kLatencyCapFrames) {
        if (++spins == 500)
            LOG_WARN("audio: backpressure spin 500 (frontier=%llu read=%llu)",
                     static_cast<unsigned long long>(frontier_),
                     static_cast<unsigned long long>(masterRead_));
        ++backpressureWaits_;
        // Guest-cooperative 1 ms slices; wake on exit/shutdown. The drain
        // (frame clock / SDL / immediate) owns the read side and runs on the
        // presenter path — we never hold the mutex while sleeping.
        if (time::TimeDomain::get().sleepUs(1000) != 0) return false;
    }
    return true;
}

i32 Mixer::output(i32 handle, const void* ptr) {
    if (handle < 1 || handle > static_cast<i32>(kMaxPorts) || !ptr) return -1;

    // ---- Phase A: wait for room WITHOUT holding the mutex across sleeps.
    // The frame-clock drain (onFramePresented) needs the mutex to advance
    // the read position while a submit is blocked — holding it here would
    // deadlock audio pacing (found in review, never shipped).
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!ports_[handle - 1].used) return -1;
    }
    const u64 waitStartUs = time::TimeDomain::get().us();
    for (;;) {
        bool room = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            Port& p = ports_[handle - 1];
            if (!p.used) return -1;              // closed while waiting
            room = frontier_ < masterRead_ + kLatencyCapFrames;
        }
        if (room || !pacingActive_) break;       // soak path never queues
        // Liveness bound: under the frame-clock drain model the presenter is
        // often the same guest thread — a submit that cannot be resolved by
        // the drain must not wait forever (SDL wall-clock devices resolve it
        // naturally; here the wait is capped and the overshoot admitted).
        if (time::TimeDomain::get().us() - waitStartUs > 50'000ull) break;
        if (!waitForRoom()) return -1;           // woken by teardown
    }

    // ---- Phase B: submit under the mutex.
    std::lock_guard<std::mutex> lk(mtx_);
    Port& p = ports_[handle - 1];
    if (!p.used) return -1;

    // Latency bookkeeping BEFORE submit (unplayed depth this submit adds to).
    const u64 depthFrames = frontier_ > masterRead_ ? frontier_ - masterRead_ : 0;
    const u32 depthMs = static_cast<u32>(depthFrames * 1000ull / kMasterRate);
    if (depthMs > maxLatencyMs_) maxLatencyMs_ = depthMs;

    dropped_ += submitToMaster(p, ptr);
    submitted_ = frontier_;

    if (!pacingActive_) {
        // Soak mode: hand everything written so far to the backend now —
        // but never past the MINIMUM live port cursor: a port that submits
        // into the current window has not written it yet, and advancing
        // past its cursor would misclassify its submit as LATE.
        u64 minCursor = frontier_;
        for (const Port& q : ports_)
            if (q.used && q.cursor < minCursor) minCursor = q.cursor;
        drain(minCursor - masterRead_);
    }
    return 0;
}

void Mixer::drain(u64 frames) {
    if (!backend_ || frames == 0) {
        // No backend: still advance the read position (audio disabled but
        // the timeline must move or paced guests would block forever).
        const u64 take = frames < (frontier_ - masterRead_) ? frames
                                                            : (frontier_ - masterRead_);
        masterRead_ += take;
        return;
    }
    if (scratch_.size() < frames * 2) scratch_.resize(frames * 2);
    u64 written = 0;
    // The read position must NEVER advance past the written frontier: the
    // gap arithmetic is unsigned and an over-advanced read position would
    // underflow every room check into "forever full" (found by the paced
    // tone run, never shipped). Unplayed authorization = underrun: the
    // frames stay pending and play on the next authorization.
    const u64 avail = frontier_ - masterRead_;
    const u64 advance = frames < avail ? frames : avail;
    for (u64 i = 0; i < advance; ++i) {
        const u64 frame = masterRead_ + i;
        const u64 idx = (frame % kRingFrames) * 2;
        auto clamp16 = [](i64 v) -> i16 {
            return static_cast<i16>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
        };
        scratch_[2 * written]     = clamp16(ring_[idx]);
        scratch_[2 * written + 1] = clamp16(ring_[idx + 1]);
        ring_[idx] = 0;
        ring_[idx + 1] = 0;
        ++written;
    }
    masterRead_ += advance;
    underrun_ += frames - advance;
    if (written) {
        backend_->feed(scratch_.data(), static_cast<u32>(written));
        mixed_ += written;
    }
}

void Mixer::onFramePresented(u64 targetFps) {
    if (targetFps == 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    pacingActive_ = true;
    // Each presented frame authorizes exactly rate/fps master frames —
    // audio consumption stays glued to video pacing.
    const u64 authorize = kMasterRate / targetFps;
    drain(authorize);
}

Mixer::Stats Mixer::stats() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stats s;
    s.submittedFrames = submitted_;
    s.mixedFrames = mixed_;
    s.droppedFrames = dropped_;
    s.underrunFrames = underrun_;
    s.maxLatencyMs = maxLatencyMs_;
    s.backpressureWaits = backpressureWaits_;
    return s;
}

const char* Mixer::backendName() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return backend_ ? backend_->name() : "none";
}

// ---------------------------------------------------------------------------
// Guest-visible AudioOut surface (extern "C", kernel export table).
// ---------------------------------------------------------------------------
extern "C" {

int32_t sceAudioOutOpen(int32_t userId, uint32_t portType, uint32_t index,
                        uint32_t lenFrames, uint32_t freq, uint32_t format) {
    if (userId != 0) {
        LOG_WARN("audio: userId %d unsupported (PSBET single-user, use 0)", userId);
        return -1;
    }
    if (portType > audio::PortVoice) {
        LOG_WARN("audio: port type %u invalid", portType);
        return -1;
    }
    return audio::Mixer::get().open(static_cast<audio::PortType>(portType),
                                    index, lenFrames, freq,
                                    static_cast<audio::Format>(format));
}

int32_t sceAudioOutOutput(int32_t handle, void* ptr) {
    return audio::Mixer::get().output(handle, ptr);
}

int32_t sceAudioOutSetVolume(int32_t handle, uint32_t chBitmask, int32_t* volumes) {
    if (chBitmask & ~0x3u) {
        LOG_WARN("audio: SetVolume channel bitmask 0x%x invalid", chBitmask);
        return -1;
    }
    return audio::Mixer::get().setVolume(handle, chBitmask, volumes);
}

int32_t sceAudioOutClose(int32_t handle) {
    return audio::Mixer::get().close(handle);
}

} // extern "C"

#if defined(_WIN32)
// Windows SysV->Win64 trampoline targets (w64_thunks.asm).
extern "C" {
int32_t psbXAudioOutOpen(int32_t a, uint32_t b, uint32_t c, uint32_t d,
                         uint32_t e, uint32_t f)
    { return sceAudioOutOpen(a, b, c, d, e, f); }
int32_t psbXAudioOutOutput(int32_t a, void* b)
    { return sceAudioOutOutput(a, b); }
int32_t psbXAudioOutSetVolume(int32_t a, uint32_t b, int32_t* c)
    { return sceAudioOutSetVolume(a, b, c); }
int32_t psbXAudioOutClose(int32_t a)
    { return sceAudioOutClose(a); }
}
#endif

} // namespace psb::audio
