// ============================================================================
//  PSBET — audio.h (Phase 4)
//  AudioOut device model: guest-facing audio ports mixed by the runtime into
//  one 48 kHz stereo master stream (guest-side mixing semantics, GUEST_ABI
//  §9), served by a pluggable backend:
//
//    * NullBackend    — discards (soak runs)
//    * WavDumpBackend — writes the mixed master as a WAV file (CI evidence)
//    * SdlBackend     — real SDL3 playback device (optional build)
//
//  Consumption model (documented in ARCHITECTURE §12):
//    * soak mode (no pacing)      — every sceAudioOutOutput mixes + drains
//                                   immediately; nothing is ever dropped.
//    * paced mode (--fps N)       — the mixer drains on frame presentation:
//                                   each presented frame authorizes exactly
//                                   (48000/N) master frames (audio glued to
//                                   video pacing — the measured-latency path).
//    * SDL backend                — the device callback drains by wall clock.
//
//  Backpressure: a port submitting while the master queue exceeds the
//  latency cap blocks (in guest-cooperative slices) until there is room.
//  The queue can therefore never overflow — dropped samples are an
//  invariant, asserted as zero everywhere.
// ============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "psbet/common.h"

namespace psb::audio {

constexpr u32 kMasterRate     = 48000;  // Hz (fixed master domain)
constexpr u32 kMasterChannels = 2;      // stereo
constexpr u32 kMaxPorts       = 8;
constexpr u32 kVolMax         = 32768;  // integer volume full scale (0..kVolMax)

// Guest-facing port formats (PSBET definition, GUEST_ABI §9).
enum Format : u32 {
    FmtS16Mono    = 0,
    FmtS16Stereo  = 1,
    FmtFloatStereo = 2,
};

// Port classes (PSBET subset; GUEST_ABI §9).
enum PortType : u32 {
    PortMain   = 0,
    PortBgm    = 1,
    PortVoice  = 2,
};

// ---------------------------------------------------------------------------
// IAudioBackend — consumes the MIXED master stream (interleaved s16 stereo).
// ---------------------------------------------------------------------------
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual const char* name() const = 0;
    // Begin consuming at the master rate/channels. false = backend failed.
    virtual bool start(u32 rate, u32 channels) = 0;
    // Stop consuming; flush any dump artifacts; join any consumer thread.
    virtual void stop() = 0;
    // Hand `frames` mixed master frames to the consumer.
    virtual void feed(const i16* interleaved, u32 frames) = 0;
    // Frames accepted but not yet played (latency measurement).
    virtual u32 pendingFrames() const { return 0; }
};

class NullBackend final : public IAudioBackend {
public:
    const char* name() const override { return "null"; }
    bool start(u32 rate, u32 channels) override;
    void stop() override;
    void feed(const i16* interleaved, u32 frames) override;
    u64 consumed() const { return consumed_; }
private:
    std::atomic<u64> consumed_{0};
};

class WavDumpBackend final : public IAudioBackend {
public:
    explicit WavDumpBackend(std::string path) : path_(std::move(path)) {}
    const char* name() const override { return "wav-dump"; }
    bool start(u32 rate, u32 channels) override;
    void stop() override;                 // writes the WAV file
    void feed(const i16* interleaved, u32 frames) override;
    u32 pendingFrames() const override { return 0; }
    const std::vector<i16>& samples() const { return samples_; }
private:
    std::string path_;
    std::vector<i16> samples_;
    u32 rate_ = 0, channels_ = 0;
    bool written_ = false;
};

#ifdef PSBET_WITH_SDL3
class SdlBackend final : public IAudioBackend {
public:
    const char* name() const override { return "sdl3"; }
    ~SdlBackend() override;
    bool start(u32 rate, u32 channels) override;
    void stop() override;
    void feed(const i16* interleaved, u32 frames) override;
    u32 pendingFrames() const override;
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
#endif

// ---------------------------------------------------------------------------
// Mixer — shared master timeline + per-port write cursors (true mixing).
//
// Every port writes (volume-applied, format-converted) into ONE master ring
// at its own cursor; ports submitting in the same frame window therefore
// SUM into the same samples (Orbis-style parallel mixing), while staggered
// submits naturally lay out sequentially. The drain side consumes from the
// master read position and resets consumed cells, so the ring is reusable.
//
//   * soak mode (no pacing)  — every sceAudioOutOutput drains the whole
//     written region immediately; nothing queues, nothing drops.
//   * paced mode (--fps N)   — the mixer drains on frame presentation:
//     each presented frame authorizes exactly (48000/N) master frames
//     (audio glued to video pacing; the measured-latency path).
//   * SDL backend            — same paced/soak rules; SDL consumes by wall
//     clock on interactive audio devices.
//
// Backpressure: a submit blocks (guest-cooperative slices) while the
// written-but-unplayed frontier is at/over the latency cap. One submit of
// overshoot is tolerated so a single large buffer can never deadlock against
// the drain. A port that submits into an already-played region is LATE —
// those frames are counted in droppedFrames (the only path that can drop).
// ---------------------------------------------------------------------------
class Mixer {
public:
    static Mixer& get();

    // Engine-side lifecycle. setBackend adopts and starts the master stream;
    // clearBackend stops it, flushes dumps and resets the timeline.
    void setBackend(std::unique_ptr<IAudioBackend> backend);
    void clearBackend();

    // Guest surface (GUEST_ABI §9). Returns a positive handle or -1 (and
    // logs the reason) on every documented failure.
    i32 open(PortType type, u32 index, u32 lenFrames, u32 freq, Format fmt);
    i32 output(i32 handle, const void* ptr);                 // submit frames
    i32 setVolume(i32 handle, u32 chBitmask, const i32* volumes); // per-ch
    i32 close(i32 handle);

    // Engine-side: one presented frame authorized (rate/fps) master frames.
    void onFramePresented(u64 targetFps);

    struct Stats {
        u64 submittedFrames = 0;     // unique master frames written (frontier)
        u64 mixedFrames     = 0;     // master frames handed to the backend
        u64 droppedFrames   = 0;     // LATE submits only (invariant 0 in CI)
        u64 underrunFrames  = 0;     // drained-before-written (silence padding)
        u32 maxLatencyMs    = 0;     // max frontier-vs-read depth at submit
        u32 backpressureWaits = 0;   // submits that blocked on the cap
    };
    Stats stats() const;

    const char* backendName() const;

private:
    Mixer() = default;

    struct Port {
        bool used = false;
        PortType type{};
        Format fmt{};
        u32 lenFrames = 0;
        u32 freq = kMasterRate;
        i32 volL = kVolMax, volR = kVolMax;
        u64 cursor = 0;              // master-frame position (this port)
    };

    // Writes one port submit into the master ring (convert + volume + sum).
    // Returns master frames dropped as LATE (0 for well-behaved guests).
    u64 submitToMaster(Port& p, const void* ptr);

    // Blocks the caller (guest-cooperative slices) while the unplayed
    // frontier is at/over the cap. Returns false if teardown requested a wake.
    bool waitForRoom();

    // Consumes up to `frames` master frames from the read position into the
    // backend (zero-padding silence when the frontier is not there yet).
    void drain(u64 frames);

    static constexpr u64 kRingFrames = 1u << 16;   // 65536 frames (~1.36 s)
    static constexpr u32 kLatencyCapMs = 30;       // backpressure cap (< 40 ms)
    static constexpr u64 kLatencyCapFrames = kLatencyCapMs * kMasterRate / 1000;
    static constexpr u32 kMaxSubmitFrames = 16384; // sanity bound per submit

    mutable std::mutex mtx_;
    Port ports_[kMaxPorts]{};
    std::unique_ptr<IAudioBackend> backend_;

    std::vector<i32> ring_;          // interleaved stereo accumulator
    u64 masterRead_ = 0;             // next master frame to play
    u64 frontier_ = 0;               // highest master frame written
    u64 submitted_ = 0;              // == frontier_ snapshot for stats
    u64 mixed_ = 0;
    u64 dropped_ = 0;
    u64 underrun_ = 0;
    u32 maxLatencyMs_ = 0;
    u32 backpressureWaits_ = 0;
    bool pacingActive_ = false;      // set when onFramePresented drives pulls
    std::vector<i16> scratch_;       // contiguous feed buffer
};

// WAV writer used by the dump backend (also unit-tested directly).
bool writeWavFile(const char* path, u32 rate, u32 channels,
                  const i16* samples, u64 sampleCount);

// Guest-visible AudioOut surface (extern "C", kernel export table).
extern "C" {
int32_t sceAudioOutOpen(int32_t userId, uint32_t portType, uint32_t index,
                        uint32_t lenFrames, uint32_t freq, uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, void* ptr);
int32_t sceAudioOutSetVolume(int32_t handle, uint32_t chBitmask,
                             int32_t* volumes);
int32_t sceAudioOutClose(int32_t handle);
}

} // namespace psb::audio
