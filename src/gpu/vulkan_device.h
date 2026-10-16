// ============================================================================
//  PSBET — vulkan_device.h
//  Hardware GPU backend: executes the same PBFF drawing surface on Vulkan.
//
//  Design:
//   - Loader-based (dlopen/LoadLibrary "vulkan-1") — no link-time SDK dep.
//   - Fully offscreen: renders into Vulkan images, reads back to the RGBA8
//     host buffer each submit. No swapchain, no window — works headless and
//     under lavapipe/SwiftShader (CI) as well as on real GPUs.
//   - Identical semantics to the software rasterizer (the reference):
//     NDC y-up with viewport y-flip in the vertex shader, z = depth,
//     CCW-in-y-down-screen-space front faces, LESS-family depth testing.
//   - Pipelines are cached (state-keyed) and so are the emitted SPIR-V
//     shaders (shader cache, disk-backed).
//   - Any fatal Vulkan error degrades to `available() == false` at creation;
//     the factory then reports unavailable instead of half-working.
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "psbet/common.h"
#include "gpu/rasterizer.h"   // IRasterBackend, DepthFunc, CullMode

namespace psb::gpu {

class VulkanDevice final : public IRasterBackend {
public:
    // Probes the machine (without creating anything): returns a short report
    // for `psbet info` — loader presence, physical devices, driver names.
    static std::string probeReport();

    // True if the loader can initialize and a capable device exists.
    static bool available();

    // Full end-to-end self-test: creates a device, renders a known triangle,
    // reads it back and verifies pixels. Signal-guarded: a driver fault is
    // reported as a failure, never a crash. Used before trusting the backend
    // with a real guest run (catches driver/JIT breakage that survives
    // creation).
    static bool selfTest();

    // Creates a fully-initialized device rendering into `colorBuf` (w*h*4
    // RGBA8). Returns nullptr with a logged reason on failure.
    static std::unique_ptr<IRasterBackend> create(u8* colorBuf, u32 width, u32 height);

    ~VulkanDevice() override;

    void setViewport(float x, float y, float w, float h) override;
    void setDepthTest(bool enable, DepthFunc func) override;
    void setCullMode(CullMode mode) override;
    void clear(float r, float g, float b, float a) override;
    void drawTriangles(const GpuVertex* v, u32 count) override;
    void frameEnd() override;

private:
    VulkanDevice() = default;
    static bool selfTestImpl();   // unguarded body used by selfTest()
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace psb::gpu
