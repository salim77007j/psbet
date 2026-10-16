// ============================================================================
//  PSBET — gpu_shim.cpp
// ============================================================================
#include "gpu_shim.h"

#include <cstring>

#include "core/log.h"
#include "core/kernel_shim.h"

namespace psb::gpu {

namespace {
Device* g_device = nullptr;
}

Device::Device(u8* colorBuf, float* depthBuf, u32 width, u32 height)
    : color_(colorBuf), width_(width), height_(height) {
    backend_ = std::make_unique<Rasterizer>(colorBuf, depthBuf, width, height);
    backendName_ = "software";
}

Device::Device(u8* colorBuf, u32 width, u32 height,
               std::unique_ptr<IRasterBackend> backend)
    : color_(colorBuf), width_(width), height_(height), backend_(std::move(backend)) {
    backendName_ = "vulkan"; // the only non-software backend in Phase 2
}

Device::~Device() = default;

void setActiveDevice(Device* device) { g_device = device; }
Device* activeDevice()               { return g_device; }

bool Device::executeCommandBuffer(const void* data, u32 sizeBytes) {
    const u8* cursor = static_cast<const u8*>(data);
    const u8* end    = cursor + sizeBytes;

    while (cursor + sizeof(CmdHeader) <= end) {
        CmdHeader hdr;
        std::memcpy(&hdr, cursor, sizeof(hdr));
        if (hdr.size < sizeof(CmdHeader) || cursor + hdr.size > end) {
            LOG_ERROR("GPU: malformed command (op=%u size=%u at %zu bytes remaining)",
                      static_cast<u32>(hdr.op), hdr.size,
                      static_cast<size_t>(end - cursor));
            return false;
        }

        switch (hdr.op) {
            case CmdOp::Nop:
                break;

            case CmdOp::Clear: {
                if (hdr.size < sizeof(CmdClear)) return false;
                CmdClear pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                backend_->clear(pkt.color[0], pkt.color[1], pkt.color[2], pkt.color[3]);
                break;
            }

            case CmdOp::SetViewport: {
                if (hdr.size < sizeof(CmdSetViewport)) return false;
                CmdSetViewport pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                backend_->setViewport(pkt.x, pkt.y, pkt.w, pkt.h);
                break;
            }

            case CmdOp::SetDepthTest: {
                if (hdr.size < sizeof(CmdSetDepthTest)) return false;
                CmdSetDepthTest pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                backend_->setDepthTest(pkt.enable != 0, pkt.func);
                break;
            }

            case CmdOp::SetCullMode: {
                if (hdr.size < sizeof(CmdSetCullMode)) return false;
                CmdSetCullMode pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                backend_->setCullMode(pkt.mode);
                break;
            }

            case CmdOp::DrawTriangles: {
                if (hdr.size < sizeof(CmdDrawTriangles)) return false;
                CmdDrawTriangles pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                const size_t want = sizeof(CmdDrawTriangles)
                                  + static_cast<size_t>(pkt.vertexCount) * sizeof(GpuVertex);
                if (hdr.size < want) {
                    LOG_ERROR("GPU: DrawTriangles payload truncated (%u < %zu)",
                              hdr.size, want);
                    return false;
                }
                // Copy vertices into a local buffer to avoid alignment issues.
                std::vector<GpuVertex> verts(pkt.vertexCount);
                std::memcpy(verts.data(), cursor + sizeof(CmdDrawTriangles),
                            static_cast<size_t>(pkt.vertexCount) * sizeof(GpuVertex));
                backend_->drawTriangles(verts.data(), pkt.vertexCount);
                break;
            }

            default:
                LOG_ERROR("GPU: unknown opcode %u — aborting buffer",
                          static_cast<u32>(hdr.op));
                return false;
        }

        cursor += hdr.size;
    }

    backend_->frameEnd(); // Vulkan: submit + readback; software: no-op
    return true;
}

void Device::completeFrame() {
    frames_++;
    if (hook_) hook_(frames_);
    if (frames_ >= maxFrames_ && !frameLimitHit_) {
        frameLimitHit_ = true;
        LOG_WARN("GPU: frame limit %llu reached — requesting guest exit(124)",
                 static_cast<unsigned long long>(maxFrames_));
        kernel::KernelState::get().requestProcessExit(124);
    }
}

extern "C" int32_t sceGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                              uint32_t* dcbSizesInBytes,
                                              void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                              uint32_t flags) {
    (void)ccbAddrs; (void)ccbSizesInBytes; (void)flags; // Phase 1: dcb only

    Device* dev = activeDevice();
    if (!dev) {
        LOG_ERROR("GPU: submit with no active device");
        return -1;
    }
    if (!dcbAddrs || !dcbSizesInBytes || count == 0) return -1;

    for (u32 i = 0; i < count; ++i) {
        if (!dcbAddrs[i] || dcbSizesInBytes[i] == 0) return -1;
        if (!dev->executeCommandBuffer(dcbAddrs[i], dcbSizesInBytes[i]))
            return -1;
    }

    dev->completeFrame();
    return 0;
}

namespace {
const struct { const char* name; void* fn; } kGpuExports[] = {
    {"sceGnmSubmitCommandBuffers", (void*)&sceGnmSubmitCommandBuffers},
};
} // namespace

void* findGpuExport(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kGpuExports)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

} // namespace psb::gpu
