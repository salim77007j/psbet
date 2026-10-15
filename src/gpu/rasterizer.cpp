// ============================================================================
//  PSBET — rasterizer.cpp
//  Edge-function rasterization with watertight fill (E >= 0), depth test and
//  barycentric color interpolation. Precision: double for edge accumulation.
//
//  Fill-rule note (v1): we accept E >= 0 for all triangles rather than a
//  strict top-left rule. Shared-edge pixels are double-drawn; with opaque
//  geometry and LESS depth testing the result is visually identical and the
//  implementation is trivially watertight. A strict rule arrives with the
//  Vulkan backend's hardware rasterizer in Phase 2.
// ============================================================================
#include "rasterizer.h"

#include <algorithm>
#include <cmath>

namespace psb::gpu {

Rasterizer::Rasterizer(u8* colorBuf, float* depthBuf, u32 width, u32 height)
    : color_(colorBuf), depth_(depthBuf), width_(width), height_(height) {
    vpW_ = static_cast<float>(width);
    vpH_ = static_cast<float>(height);
}

void Rasterizer::setViewport(float x, float y, float w, float h) {
    vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h;
}

void Rasterizer::setDepthTest(bool enable, DepthFunc func) {
    depthTest_ = enable;
    depthFunc_ = func;
}

void Rasterizer::setCullMode(CullMode mode) { cull_ = mode; }

void Rasterizer::clear(float r, float g, float b, float a) {
    const u32 packed = static_cast<u32>(std::clamp(r, 0.f, 1.f) * 255.99f)
                     | (static_cast<u32>(std::clamp(g, 0.f, 1.f) * 255.99f) << 8)
                     | (static_cast<u32>(std::clamp(b, 0.f, 1.f) * 255.99f) << 16)
                     | (static_cast<u32>(std::clamp(a, 0.f, 1.f) * 255.99f) << 24);
    u32* px = reinterpret_cast<u32*>(color_);
    std::fill(px, px + static_cast<size_t>(width_) * height_, packed);
    std::fill(depth_, depth_ + static_cast<size_t>(width_) * height_, 1.0f);
}

void Rasterizer::drawTriangles(const GpuVertex* v, u32 count) {
    for (u32 t = 0; t + 3 <= count; t += 3) {
        const GpuVertex& a = v[t + 0];
        const GpuVertex& b = v[t + 1];
        const GpuVertex& c = v[t + 2];

        // NDC -> screen (y-up NDC, y-down screen). z maps directly to depth.
        const double ax = vpX_ + (a.pos[0] * 0.5 + 0.5) * vpW_;
        const double ay = vpY_ + (1.0 - (a.pos[1] * 0.5 + 0.5)) * vpH_;
        const double bx = vpX_ + (b.pos[0] * 0.5 + 0.5) * vpW_;
        const double by = vpY_ + (1.0 - (b.pos[1] * 0.5 + 0.5)) * vpH_;
        const double cx = vpX_ + (c.pos[0] * 0.5 + 0.5) * vpW_;
        const double cy = vpY_ + (1.0 - (c.pos[1] * 0.5 + 0.5)) * vpH_;

        const double area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (area2 == 0.0) continue;

        // Backface culling: positive area2 = counter-clockwise in y-down
        // screen space = front face (matches our cube winding).
        if (cull_ == CullMode::Back && area2 <= 0.0) continue;

        const double invArea = 1.0 / area2;

        // Bounding box clamped to viewport and framebuffer.
        const i32 minX = std::max<i32>(static_cast<i32>(vpX_),
            std::max<i32>(0, static_cast<i32>(std::floor(std::min({ax, bx, cx})))));
        const i32 maxX = std::min<i32>(static_cast<i32>(vpX_ + vpW_) - 1,
            std::min<i32>(static_cast<i32>(width_) - 1,
                          static_cast<i32>(std::ceil(std::max({ax, bx, cx})))));
        const i32 minY = std::max<i32>(static_cast<i32>(vpY_),
            std::max<i32>(0, static_cast<i32>(std::floor(std::min({ay, by, cy})))));
        const i32 maxY = std::min<i32>(static_cast<i32>(vpY_ + vpH_) - 1,
            std::min<i32>(static_cast<i32>(height_) - 1,
                          static_cast<i32>(std::ceil(std::max({ay, by, cy})))));
        if (minX > maxX || minY > maxY) continue;

        for (i32 py = minY; py <= maxY; ++py) {
            for (i32 px = minX; px <= maxX; ++px) {
                const double sx = px + 0.5;
                const double sy = py + 0.5;

                const double w0 = ((bx - ax) * (sy - ay) - (by - ay) * (sx - ax)) * invArea;
                const double w1 = ((cx - bx) * (sy - by) - (cy - by) * (sx - bx)) * invArea;
                const double w2 = 1.0 - w0 - w1;

                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;

                const double depth = a.pos[2] * w0 + b.pos[2] * w1 + c.pos[2] * w2;
                const size_t idx = static_cast<size_t>(py) * width_ + px;

                if (depthTest_) {
                    bool pass = false;
                    switch (depthFunc_) {
                        case DepthFunc::Never: pass = false; break;
                        case DepthFunc::Less:  pass = depth <  depth_[idx]; break;
                        case DepthFunc::Equal: pass = depth == depth_[idx]; break;
                        case DepthFunc::LEqual:pass = depth <= depth_[idx]; break;
                    }
                    if (!pass) continue;
                }

                depth_[idx] = static_cast<float>(depth);

                const float r = static_cast<float>(a.col[0] * w0 + b.col[0] * w1 + c.col[0] * w2);
                const float g = static_cast<float>(a.col[1] * w0 + b.col[1] * w1 + c.col[1] * w2);
                const float bl= static_cast<float>(a.col[2] * w0 + b.col[2] * w1 + c.col[2] * w2);
                const float al= static_cast<float>(a.col[3] * w0 + b.col[3] * w1 + c.col[3] * w2);

                u8* out = color_ + idx * 4;
                out[0] = static_cast<u8>(std::clamp(r, 0.f, 1.f) * 255.99f);
                out[1] = static_cast<u8>(std::clamp(g, 0.f, 1.f) * 255.99f);
                out[2] = static_cast<u8>(std::clamp(bl, 0.f, 1.f) * 255.99f);
                out[3] = static_cast<u8>(std::clamp(al, 0.f, 1.f) * 255.99f);
            }
        }
    }
}

} // namespace psb::gpu
