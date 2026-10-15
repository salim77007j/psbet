// ============================================================================
//  PSBET — rasterizer.h
//  Software rasterizer (Phase 1 GPU backend). Produces a RGBA8 color buffer
//  plus a float32 depth buffer. Zero dependencies, deterministic output,
//  runs headless (CI-friendly) — used as the reference backend until the
//  Vulkan backend lands in Phase 2.
// ============================================================================
#pragma once

#include "psbet/common.h"
#include "gpu/cmd_stream.h"

namespace psb::gpu {

class Rasterizer {
public:
    // `colorBuf` is w*h*4 bytes RGBA8; `depthBuf` is w*h floats. The device
    // owns the storage; the rasterizer only draws into it.
    Rasterizer(u8* colorBuf, float* depthBuf, u32 width, u32 height);

    void setViewport(float x, float y, float w, float h);
    void setDepthTest(bool enable, DepthFunc func);
    void setCullMode(CullMode mode);
    void clear(float r, float g, float b, float a);
    void drawTriangles(const GpuVertex* v, u32 count);

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
