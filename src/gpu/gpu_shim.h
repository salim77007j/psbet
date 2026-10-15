// ============================================================================
//  PSBET — gpu_shim.h
//  GPU device (software backend) + guest-facing submit interface.
// ============================================================================
#pragma once

#include "psbet/common.h"
#include "gpu/cmd_stream.h"
#include "gpu/rasterizer.h"

#include <functional>
#include <string>

namespace psb::gpu {

// ---------------------------------------------------------------------------
// Device — owns a framebuffer + depth buffer and executes guest command
// buffers on the software rasterizer. In Phase 2 this becomes the Vulkan
// translation layer; the software backend remains as the reference device.
// ---------------------------------------------------------------------------
class Device {
public:
    using FrameHook = std::function<void(u64 frameNumber)>;

    Device(u8* colorBuf, float* depthBuf, u32 width, u32 height);

    // Executes one display command buffer. Returns false on malformed input
    // (invalid opcodes/sizes) — the error is logged, execution stops.
    bool executeCommandBuffer(const void* data, u32 sizeBytes);

    u64 frameCount() const { return frames_; }
    void setFrameHook(FrameHook hook) { hook_ = std::move(hook); }
    void setMaxFrames(u64 max) { maxFrames_ = max; }
    u64 maxFrames() const { return maxFrames_; }
    bool frameLimitHit() const { return frameLimitHit_; }

    // Called once per completed guest submit: bumps the frame counter, runs
    // the frame hook, and enforces the safety frame limit (requests guest
    // exit(124) when hit).
    void completeFrame();

    u32 width() const { return width_; }
    u32 height() const { return height_; }

private:
    u8*    color_;
    float* depth_;
    u32    width_;
    u32    height_;
    Rasterizer raster_;

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

// The active device used by the shim (set by the Emulator before guest load).
void setActiveDevice(Device* device);
Device* activeDevice();

// Import-table lookup for the ELF loader (names -> host fn pointers).
void* findGpuExport(const char* name);

} // namespace psb::gpu
