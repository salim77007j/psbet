// ============================================================================
//  PSBET — gpu_shim.cpp
// ============================================================================
#include "gpu_shim.h"

#include <cstring>

#include "core/log.h"
#include "core/kernel_shim.h"
#include "core/memory.h"
#include "core/guest_vm.h"
#include "psbet/psb_guest_api.h"

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

            case CmdOp::DrawTrianglesGpu: {
                // v1.1: vertices live at a GPU virtual address. Translate
                // through the GPU page tables per submit — an unmapped or
                // out-of-window address rejects the buffer.
                if (hdr.size < sizeof(CmdDrawTrianglesGpu)) return false;
                CmdDrawTrianglesGpu pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                if (!addressSpace_) {
                    LOG_ERROR("GPU: GPU-VA draw without an address space");
                    return false;
                }
                const size_t wantBytes = static_cast<size_t>(pkt.vertexCount)
                                       * sizeof(GpuVertex);
                // Translate the FIRST byte, then verify the whole span is
                // backed by contiguous physical pages (page-table walk).
                u8* host = static_cast<u8*>(addressSpace_->gpuTranslate(pkt.gpuVa));
                if (!host) {
                    LOG_ERROR("GPU: GPU-VA draw at 0x%llx — unmapped",
                              static_cast<unsigned long long>(pkt.gpuVa));
                    return false;
                }
                for (u64 off = 0; off < wantBytes; off += 0x1000) {
                    u64 pa = 0;
                    const u64 page = pkt.gpuVa + off;
                    if (!addressSpace_->gpuTranslateRaw(page, &pa)) {
                        LOG_ERROR("GPU: GPU-VA draw span unmapped at 0x%llx",
                                  static_cast<unsigned long long>(page));
                        return false;
                    }
                }
                backend_->drawTriangles(reinterpret_cast<GpuVertex*>(host),
                                        pkt.vertexCount);
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

#if defined(_WIN32)
// Windows SysV->Win64 trampoline target (w64_thunks.asm).
extern "C" int32_t psbXGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                               uint32_t* dcbSizesInBytes,
                                               void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                               uint32_t flags) {
    return sceGnmSubmitCommandBuffers(count, dcbAddrs, dcbSizesInBytes,
                                      ccbAddrs, ccbSizesInBytes, flags);
}
extern "C" int32_t psbXGnmReserveGpuAddress(uint64_t a, uint64_t b, uint64_t c, uint64_t* d) {
    return sceGnmReserveGpuAddress(a, b, c, d);
}
extern "C" int32_t psbXGnmMapGpuAddress(uint64_t a, int64_t b, uint64_t c, uint32_t d) {
    return sceGnmMapGpuAddress(a, b, c, d);
}
extern "C" int32_t psbXGnmUnmapGpuAddress(uint64_t a, uint64_t b) {
    return sceGnmUnmapGpuAddress(a, b);
}
#endif

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

// ---- GPU virtual memory (v1.2) ---------------------------------------------
extern "C" int32_t sceGnmReserveGpuAddress(uint64_t len, uint64_t alignment,
                                           uint64_t fixedGpuVa, uint64_t* gpuVaOut) {
    if (!gpuVaOut || len == 0) return -1;
    kernel::KernelState& k = kernel::KernelState::get();
    const u64 va = k.memory()->guest().gpuReserve(len, alignment, fixedGpuVa);
    if (!va) return -1;
    *gpuVaOut = va;
    return 0;
}

extern "C" int32_t sceGnmMapGpuAddress(uint64_t gpuVa, int64_t physAddr,
                                       uint64_t len, uint32_t prot) {
    if (physAddr <= 0 || len == 0) return -1;
    kernel::KernelState& k = kernel::KernelState::get();
    vm::Prot p = vm::Prot::ReadOnly;
    if (prot & PSB_PROT_WRITE) p = vm::Prot::ReadWrite;
    return k.memory()->guest().gpuMap(gpuVa, static_cast<u64>(physAddr), len, p)
        ? 0 : -1;
}

extern "C" int32_t sceGnmUnmapGpuAddress(uint64_t gpuVa, uint64_t len) {
    if (len == 0) return -1;
    kernel::KernelState& k = kernel::KernelState::get();
    return k.memory()->guest().gpuUnmap(gpuVa, len) ? 0 : -1;
}

namespace {
const struct { const char* name; void* fn; } kGpuExports[] = {
    {"sceGnmSubmitCommandBuffers", (void*)&sceGnmSubmitCommandBuffers},
    {"sceGnmReserveGpuAddress",    (void*)&sceGnmReserveGpuAddress},
    {"sceGnmMapGpuAddress",        (void*)&sceGnmMapGpuAddress},
    {"sceGnmUnmapGpuAddress",      (void*)&sceGnmUnmapGpuAddress},
};
} // namespace

void* findGpuExport(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kGpuExports)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

} // namespace psb::gpu
