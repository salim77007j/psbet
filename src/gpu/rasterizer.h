// ============================================================================
//  PSBET — rasterizer.h
//  Software rasterizer (reference GPU backend). Produces a RGBA8 color buffer
//  plus a float32 depth buffer. Zero dependencies, deterministic output,
//  runs headless (CI-friendly) — the correctness oracle for the Vulkan
//  backend and the permanent fallback device.
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
    virtual void frameEnd() { (void)0; }
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
