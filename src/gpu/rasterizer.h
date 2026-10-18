// ============================================================================
//  PSBET — rasterizer.h
//  Software rasterizer (reference GPU backend). Produces a RGBA8 color buffer
//  plus a float32 depth buffer. Zero dependencies, deterministic output,
//  runs headless (CI-friendly) — the correctness oracle for the Vulkan
//  backend and the permanent fallback device.
//
//  Phase 5 performance model:
//    * Row-band parallel execution — a triangle's bounding box is split
//      into disjoint row bands executed on a persistent worker pool.
//      Bands own disjoint pixel sets, so output is BYTE-IDENTICAL to the
//      single-thread path for any thread count (locked by test).
//    * Optional AVX2 fast path (x86-64, GCC/Clang): the same IEEE double
//      arithmetic, four pixels per vector, explicit intrinsics (no FMA
//      contraction) — BYTE-IDENTICAL to the scalar path (locked by test).
//    * The scalar double path remains the reference; SIMD/tiles are pure
//      scheduling. Runtime dispatch + setters for tests and tuning.
// ============================================================================
#pragma once

#include "psbet/common.h"
#include "gpu/cmd_stream.h"

namespace psb::gpu {

// Common drawing interface implemented by every GPU backend (software
// rasterizer, Vulkan device). The command executor (Device) drives exactly
// this surface, so backends are swappable per run.
class IRasterBackend {
public:
    virtual ~IRasterBackend() = default;

    // `colorBuf` is w*h*4 bytes RGBA8; `depthBuf` is w*h floats. The software
    // backend draws into them directly; hardware-backed devices treat them as
    // the presentation target (color is copied back after each submit).
    virtual void setViewport(float x, float y, float w, float h) = 0;
    virtual void setDepthTest(bool enable, DepthFunc func) = 0;
    virtual void setCullMode(CullMode mode) = 0;
    virtual void clear(float r, float g, float b, float a) = 0;
    virtual void drawTriangles(const GpuVertex* v, u32 count) = 0;

    // Called once after each command-buffer execution (one guest submit = one
    // frame). Hardware backends flush/submit/readback here; the software
    // rasterizer needs nothing. Default: no-op.
    // Phase 5: `readback` tells hardware backends whether the host color
    // buffer is actually consumed this frame (screenshots / window present).
    // Skipping the readback on unconsumed frames removes a full framebuffer
    // copy per submit. The software backend ignores it.
    virtual void frameEnd(bool readback) { (void)readback; }
};

class Rasterizer final : public IRasterBackend {
public:
    // The device owns the storage; the rasterizer only draws into it.
    Rasterizer(u8* colorBuf, float* depthBuf, u32 width, u32 height);

    void setViewport(float x, float y, float w, float h) override;
    void setDepthTest(bool enable, DepthFunc func) override;
    void setCullMode(CullMode mode) override;
    void clear(float r, float g, float b, float a) override;
    void drawTriangles(const GpuVertex* v, u32 count) override;

    // ---- Phase 5 tuning (process-global; used by tests + the bench tool) --
    // Band worker count (0 = auto: min(hw_threads - 1, 8), floor 1). The
    // PSBET_RASTER_THREADS env var overrides the auto default at first use.
    static void setBandThreads(u32 n);
    static u32  bandThreads();
    // AVX fast-path toggle (default: on when the host CPU supports AVX2 and
    // the compiler emitted the path). Byte-identical output either way.
    static void setSimdEnabled(bool on);
    static bool simdEnabled();
    static bool simdCompiled();          // true when the AVX path exists
    static bool hostHasAvx2();           // CPUID-based runtime check

    // Band-core accessors (internal use; exposed for testability).
    u32    width()  const { return width_; }
    u32    height() const { return height_; }
    const float* viewport() const { return &vpX_; }
    u8*    colorData() const { return color_; }
    float* depthData() const { return depth_; }

private:
    u8*    color_;
    float* depth_;
    u32    width_;
    u32    height_;

    float  vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    bool   depthTest_ = false;
    DepthFunc depthFunc_ = DepthFunc::Less;
    CullMode cull_ = CullMode::None;
};

} // namespace psb::gpu
