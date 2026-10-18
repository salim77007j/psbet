// ============================================================================
//  PSBET — gpu_shim.h
//  GPU device (backend driver) + guest-facing submit interface.
//
//  Phase 5: the Device owns two asynchronous command queues (graphics +
//  compute). In pipelined mode a guest submit snapshots the command buffer,
//  enqueues it, and returns — a dedicated worker executes while the guest
//  builds the next frame (console submit semantics). `--gpu-sync` restores
//  inline execution. Cross-queue ordering is the guest's responsibility
//  (sceGnmWaitQueueIdle), exactly as on the console.
// ============================================================================
#pragma once

#include "psbet/common.h"
#include "gpu/cmd_stream.h"
#include "gpu/rasterizer.h"
#include "gpu/gpu_queue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace psb {
class GuestMemory;
namespace av { class FramePacer; class IPresenter; }
namespace audio { class Mixer; }
}

namespace psb::gpu {

// ---------------------------------------------------------------------------
// Device — owns a framebuffer + depth buffer and executes guest command
// buffers on the active backend (software rasterizer or Vulkan). Parsing and
// validation are shared; the backend only implements the drawing surface.
// ---------------------------------------------------------------------------
class Device {
public:
    using FrameHook = std::function<void(u64 frameNumber)>;

    // Software-backend constructor (compatibility; used by unit tests).
    Device(u8* colorBuf, float* depthBuf, u32 width, u32 height);

    // Explicit-backend constructor (Vulkan device or any future backend).
    Device(u8* colorBuf, u32 width, u32 height,
           std::unique_ptr<IRasterBackend> backend);

    ~Device();

    // Executes one display command buffer inline. Returns false on malformed
    // input (invalid opcodes/sizes) — the error is logged, execution stops.
    // This is the synchronous core; it is also the GPU workers' entry point.
    bool executeCommandBuffer(const void* data, u32 sizeBytes);

    u64 frameCount() const { return frames_; }
    void setFrameHook(FrameHook hook) { hook_ = std::move(hook); }
    void setMaxFrames(u64 max) { maxFrames_ = max; }
    u64 maxFrames() const { return maxFrames_; }
    bool frameLimitHit() const { return frameLimitHit_; }

    // Phase-3 GPU VM: the address space used to translate GPU-VA draws.
    void setAddressSpace(GuestMemory* space) { addressSpace_ = space; }

    // Phase-4 frame clock: pacer (target-FPS authorization / vsync) and the
    // audio mixer (frame-clock audio pulls). Null = free run, no audio pull.
    void setFrameClock(av::FramePacer* pacer, audio::Mixer* mixer);
    // Phase-4 presentation sink (offscreen by default).
    void setPresenter(av::IPresenter* presenter);

    // ---- Phase 5 -----------------------------------------------------------
    // Pipelined submit mode (console semantics): default OFF so direct
    // Device users (unit tests) keep synchronous semantics; the Emulator
    // enables it (and --gpu-sync disables it again).
    void setPipelined(bool on);
    bool pipelined() const { return pipelined_; }
    // Host color buffer consumed this frame (window present / screenshots)?
    // Hardware backends skip the per-frame readback when it is not.
    // Default TRUE (old semantics: always read back) — direct Device users
    // (pixel-compare oracle, self-tests) depend on the copy. The Emulator
    // sets it false only when nothing consumes the buffer this run.
    void setReadbackHint(bool on) { readbackHint_ = on; }
    // Drain + stop both queues (idempotent). The Emulator calls this before
    // reading final stats; ~Device calls it again harmlessly.
    void shutdownQueues();
    // Queue statistics for the run summary.
    struct QueueStats {
        GpuQueue::Stats gfx;
        GpuQueue::Stats compute;
        bool pipelined = false;
    };
    QueueStats queueStats() const;

    // Called once per completed guest submit: bumps the frame counter, runs
    // the frame hook, and enforces the safety frame limit (sets the limit
    // flag; the guest-side submit path turns it into the process exit).
    // Worker-thread safe (Phase 5: invoked from the GPU queue workers).
    void completeFrame();

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    const char* backendName() const { return backendName_; }

private:
    static bool execTrampoline(void* self, const void* data, u32 size,
                               bool lastInSubmit);
public:
    // Guest-submit helpers (used by the sceGnm* shims below; also the GPU
    // workers' entry point). Guest-side frame-limit check turns the
    // worker-set limit flag into the process exit request.
    bool executeItem(const void* data, u32 sizeBytes, bool lastInSubmit);
    bool validateCommandBuffer(const void* data, u32 sizeBytes) const;
    void checkFrameLimitFromGuest();

private:
    // Fast-path validation helper shared by the two submit shims.
    bool validateChain(const void* data, u32 sizeBytes) const;

    u8*    color_;
    u32    width_;
    u32    height_;
    std::unique_ptr<IRasterBackend> backend_;
    const char* backendName_ = "software";
    GuestMemory* addressSpace_ = nullptr;   // GPU VA translation (Phase 3)
    av::FramePacer* pacer_ = nullptr;       // Phase 4 frame clock
    audio::Mixer*   mixer_ = nullptr;
    av::IPresenter* presenter_ = nullptr;

    u64  frames_ = 0;
    u64  maxFrames_ = ~0ull;
    std::atomic<bool> frameLimitHit_{false};   // worker-set; guest acts on it
    bool pipelined_ = false;
    bool readbackHint_ = true;
    FrameHook hook_;

public:
    // NOTE: queues are declared LAST — destroyed FIRST — so their workers
    // never outlive the backend/buffers they touch. ~Device also shuts
    // them down explicitly before any member destruction. Public because
    // the guest-submit shims route into them directly.
    GpuQueue gfxQ_;
    GpuQueue computeQ_;
};

// Guest-visible GPU submit entry point (registered as an import).
// Mirrors the Orbis-style signature; see docs/GUEST_ABI.md.
extern "C" int32_t sceGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                              uint32_t* dcbSizesInBytes,
                                              void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                              uint32_t flags);

// Phase-5 compute queue surface (clean-room, GUEST_ABI §16):
//   sceGnmSubmitCompute  — submit command buffers to the compute queue
//   sceGnmWaitQueueIdle  — wait for queues to drain (mask: 1=gfx, 2=compute)
extern "C" int32_t sceGnmSubmitCompute(uint32_t count, void** ccbAddrs,
                                       uint32_t* ccbSizesInBytes);
extern "C" int32_t sceGnmWaitQueueIdle(uint32_t queueMask);

// Guest-visible GPU virtual-memory surface (v1.2; see docs/GUEST_ABI.md §9).
extern "C" int32_t sceGnmReserveGpuAddress(uint64_t len, uint64_t alignment,
                                           uint64_t fixedGpuVa, uint64_t* gpuVaOut);
extern "C" int32_t sceGnmMapGpuAddress(uint64_t gpuVa, int64_t physAddr,
                                       uint64_t len, uint32_t prot);
extern "C" int32_t sceGnmUnmapGpuAddress(uint64_t gpuVa, uint64_t len);

// The active device used by the shim (set by the Emulator before guest load).
void setActiveDevice(Device* device);
Device* activeDevice();

// Import-table lookup for the ELF loader (names -> host fn pointers).
void* findGpuExport(const char* name);

} // namespace psb::gpu
