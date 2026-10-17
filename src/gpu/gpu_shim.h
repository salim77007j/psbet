// ============================================================================
//  PSBET — gpu_shim.h
//  GPU device (backend driver) + guest-facing submit interface.
// ============================================================================
#pragma once

#include "psbet/common.h"
#include "gpu/cmd_stream.h"
#include "gpu/rasterizer.h"

#include <functional>
#include <memory>
#include <string>

namespace psb {
class GuestMemory;
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

    // Executes one display command buffer. Returns false on malformed input
    // (invalid opcodes/sizes) — the error is logged, execution stops.
    bool executeCommandBuffer(const void* data, u32 sizeBytes);

    u64 frameCount() const { return frames_; }
    void setFrameHook(FrameHook hook) { hook_ = std::move(hook); }
    void setMaxFrames(u64 max) { maxFrames_ = max; }
    u64 maxFrames() const { return maxFrames_; }
    bool frameLimitHit() const { return frameLimitHit_; }

    // Phase-3 GPU VM: the address space used to translate GPU-VA draws.
    void setAddressSpace(GuestMemory* space) { addressSpace_ = space; }

    // Called once per completed guest submit: bumps the frame counter, runs
    // the frame hook, and enforces the safety frame limit (requests guest
    // exit(124) when hit).
    void completeFrame();

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    const char* backendName() const { return backendName_; }

private:
    u8*    color_;
    u32    width_;
    u32    height_;
    std::unique_ptr<IRasterBackend> backend_;
    const char* backendName_ = "software";
    GuestMemory* addressSpace_ = nullptr;   // GPU VA translation (Phase 3)

    u64  frames_ = 0;
    u64  maxFrames_ = ~0ull;
    bool frameLimitHit_ = false;
    FrameHook hook_;
};

// Guest-visible GPU submit entry point (registered as an import).
// Mirrors the Orbis-style signature; see docs/GUEST_ABI.md.
extern "C" int32_t sceGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                              uint32_t* dcbSizesInBytes,
                                              void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                              uint32_t flags);

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
