// ============================================================================
//  PSBET — cmd_stream.h
//  GPU command stream ("PBFF" — PSBET Buffer Format, v1).
//
//  Clean-room command-buffer definition. Guests build typed command packets
//  into a contiguous buffer and submit it via sceGnmSubmitCommandBuffers.
//  Phase 1 executes these on the software rasterizer; the Vulkan backend
//  (Phase 2) will execute the same packets on the host GPU.
//
//  Vertex stage contract (v1): vertices arrive in NDC (x, y, z, w) with
//  pre-transformed positions (the guest performs matrix math). z is used as
//  the depth value in [0, 1] (1 = far). w is reserved for Phase 2's clip.
// ============================================================================
#pragma once

#include <cstring>
#include <vector>
#include "psbet/common.h"

namespace psb::gpu {

enum class CmdOp : u32 {
    Nop           = 0,
    Clear         = 1,
    SetViewport   = 2,
    SetDepthTest  = 3,
    SetCullMode   = 4,
    DrawTriangles = 5,
};

enum class DepthFunc : u32 { Never = 0, Less = 1, Equal = 2, LEqual = 3 };
enum class CullMode : u32 { None = 0, Back = 1 };

#pragma pack(push, 1)

struct CmdHeader { CmdOp op; u32 size; };

struct CmdClear {
    CmdHeader hdr{CmdOp::Clear, sizeof(CmdClear)};
    float color[4]{0.f, 0.f, 0.f, 1.f};
};

struct CmdSetViewport {
    CmdHeader hdr{CmdOp::SetViewport, sizeof(CmdSetViewport)};
    float x = 0, y = 0, w = 0, h = 0;
};

struct CmdSetDepthTest {
    CmdHeader hdr{CmdOp::SetDepthTest, sizeof(CmdSetDepthTest)};
    u32 enable = 0;
    DepthFunc func = DepthFunc::Less;
};

struct CmdSetCullMode {
    CmdHeader hdr{CmdOp::SetCullMode, sizeof(CmdSetCullMode)};
    CullMode mode = CullMode::None;
};

struct GpuVertex {
    float pos[4]{0, 0, 0, 1};   // NDC, z = depth in [0,1]
    float col[4]{1, 1, 1, 1};   // RGBA 0..1
};

struct CmdDrawTriangles {
    CmdHeader hdr{CmdOp::DrawTriangles, sizeof(CmdDrawTriangles)};
    u32 vertexCount = 0;
    // followed by vertexCount * GpuVertex
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// CmdWriter — tiny helper for guests to assemble a command buffer.
// ---------------------------------------------------------------------------
class CmdWriter {
public:
    template <typename T>
    void push(const T& pkt) {
        const u8* p = reinterpret_cast<const u8*>(&pkt);
        buf_.insert(buf_.end(), p, p + sizeof(T));
    }

    void draw(const GpuVertex* v, u32 count) {
        CmdDrawTriangles d;
        d.vertexCount = count;
        d.hdr.size = static_cast<u32>(sizeof(CmdDrawTriangles) + count * sizeof(GpuVertex));
        push(d);
        const u8* p = reinterpret_cast<const u8*>(v);
        buf_.insert(buf_.end(), p, p + count * sizeof(GpuVertex));
    }

    void clear() { buf_.clear(); }
    const void* data() const { return buf_.data(); }
    u32 size() const { return static_cast<u32>(buf_.size()); }

private:
    std::vector<u8> buf_;
};

} // namespace psb::gpu
