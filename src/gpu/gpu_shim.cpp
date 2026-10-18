// ============================================================================
//  PSBET — gpu_shim.cpp (Phase 5)
//  GPU device: pipelined submit path (graphics + compute queues), the
//  synchronous compatibility path, and the guest-facing GNM surface.
// ============================================================================
#include "gpu_shim.h"

#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/kernel_shim.h"
#include "core/memory.h"
#include "core/guest_vm.h"
#include "core/pacer.h"
#include "core/audio.h"
#include "core/pad.h"
#include "psbet/psb_guest_api.h"

namespace psb::gpu {

namespace {
Device* g_device = nullptr;
constexpr u32 kQueueGraphics = 1;
constexpr u32 kQueueCompute  = 2;
}

Device::Device(u8* colorBuf, float* depthBuf, u32 width, u32 height)
    : color_(colorBuf), width_(width), height_(height),
      gfxQ_("graphics", &Device::execTrampoline, this),
      computeQ_("compute", &Device::execTrampoline, this) {
    backend_ = std::make_unique<Rasterizer>(colorBuf, depthBuf, width, height);
    backendName_ = "software";
}

Device::Device(u8* colorBuf, u32 width, u32 height,
               std::unique_ptr<IRasterBackend> backend)
    : color_(colorBuf), width_(width), height_(height),
      backend_(std::move(backend)),
      gfxQ_("graphics", &Device::execTrampoline, this),
      computeQ_("compute", &Device::execTrampoline, this) {
    backendName_ = "vulkan"; // the only non-software backend in Phase 2
}

Device::~Device() {
    shutdownQueues();
}

void setActiveDevice(Device* device) { g_device = device; }
Device* activeDevice()               { return g_device; }

void Device::setFrameClock(av::FramePacer* pacer, audio::Mixer* mixer) {
    pacer_ = pacer;
    mixer_ = mixer;
}

void Device::setPresenter(av::IPresenter* presenter) { presenter_ = presenter; }

void Device::setPipelined(bool on) {
    pipelined_ = on;
    // Blocked submits must never wait past a process exit or a frame limit.
    auto wake = [] {
        kernel::KernelState& k = kernel::KernelState::get();
        if (k.exitRequested()) return true;
        Device* d = activeDevice();
        return d && d->frameLimitHit();
    };
    gfxQ_.setWakeCondition(wake);
    computeQ_.setWakeCondition(wake);
}

void Device::shutdownQueues() {
    gfxQ_.shutdown();
    computeQ_.shutdown();
}

Device::QueueStats Device::queueStats() const {
    QueueStats s;
    s.gfx = gfxQ_.stats();
    s.compute = computeQ_.stats();
    s.pipelined = pipelined_;
    return s;
}

bool Device::execTrampoline(void* self, const void* data, u32 size,
                            bool lastInSubmit) {
    return static_cast<Device*>(self)->executeItem(data, size, lastInSubmit);
}

bool Device::executeItem(const void* data, u32 sizeBytes, bool lastInSubmit) {
    if (!executeCommandBuffer(data, sizeBytes)) return false;
    if (lastInSubmit) completeFrame();   // one submit = one frame
    return true;
}

void Device::checkFrameLimitFromGuest() {
    // Runs on the submitting guest thread (supervisor or worker): the
    // worker-side limit flag becomes the process exit — same unwind path
    // as every other guest exit.
    if (frameLimitHit_.load(std::memory_order_acquire))
        kernel::KernelState::get().requestProcessExit(124);
}

void Device::completeFrame() {
    // Phase-4 order: vsync backpressure -> count/hook/limit -> present ->
    // pace to the authorized slot -> frame-clock audio pull -> pad refresh.
    // Phase 5: runs on the GPU queue worker in pipelined mode — every call
    // below is thread-safe; the frame-limit REQUEST is only recorded here
    // and turned into a process exit on the guest thread (never longjmp
    // from a host worker).
    if (pacer_) pacer_->onFrameSubmitted();

    frames_++;
    if (hook_) hook_(frames_);
    if (frames_ >= maxFrames_ && !frameLimitHit_.exchange(true)) {
        LOG_WARN("GPU: frame limit %llu reached — requesting guest exit(124)",
                 static_cast<unsigned long long>(maxFrames_));
    }

    if (presenter_) presenter_->present(color_, width_, height_);
    if (pacer_) pacer_->onFramePresented();
    if (mixer_)
        mixer_->onFramePresented(pacer_ ? pacer_->stats().authorizedFps : 0);
    pad::PadService::get().poll();   // engine-side input refresh per frame
}

// ---------------------------------------------------------------------------
// Submit-time validation — mirrors the executor's checks exactly so a
// malformed buffer is rejected to the guest (return -1) instead of being
// logged-and-skipped on the worker after the submit already returned 0.
// ---------------------------------------------------------------------------
bool Device::validateCommandBuffer(const void* data, u32 sizeBytes) const {
    const u8* cursor = static_cast<const u8*>(data);
    const u8* end    = cursor + sizeBytes;

    while (cursor + sizeof(CmdHeader) <= end) {
        CmdHeader hdr;
        std::memcpy(&hdr, cursor, sizeof(hdr));
        if (hdr.size < sizeof(CmdHeader) || cursor + hdr.size > end) return false;

        size_t minSize = sizeof(CmdHeader);
        switch (hdr.op) {
            case CmdOp::Nop:              minSize = sizeof(CmdHeader); break;
            case CmdOp::Clear:            minSize = sizeof(CmdClear); break;
            case CmdOp::SetViewport:      minSize = sizeof(CmdSetViewport); break;
            case CmdOp::SetDepthTest:     minSize = sizeof(CmdSetDepthTest); break;
            case CmdOp::SetCullMode:      minSize = sizeof(CmdSetCullMode); break;
            case CmdOp::FillMemory:       minSize = sizeof(CmdFillMemory); break;
            case CmdOp::DrawTrianglesGpu: minSize = sizeof(CmdDrawTrianglesGpu); break;
            case CmdOp::DrawTriangles: {
                minSize = sizeof(CmdDrawTriangles);
                if (hdr.size >= minSize) {
                    CmdDrawTriangles pkt;
                    std::memcpy(&pkt, cursor, sizeof(pkt));
                    const size_t want = sizeof(CmdDrawTriangles)
                        + static_cast<size_t>(pkt.vertexCount) * sizeof(GpuVertex);
                    if (hdr.size < want) return false;
                }
                break;
            }
            default:
                return false;   // unknown opcode
        }
        if (hdr.size < minSize) return false;
        cursor += hdr.size;
    }
    return cursor == end;
}

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

            case CmdOp::FillMemory: {
                // v1.4: compute-queue fill through the GPU page tables.
                if (hdr.size < sizeof(CmdFillMemory)) return false;
                CmdFillMemory pkt;
                std::memcpy(&pkt, cursor, sizeof(pkt));
                if (!addressSpace_) {
                    LOG_ERROR("GPU: FillMemory without an address space");
                    return false;
                }
                if (pkt.byteLen == 0 || pkt.byteLen > (64ull << 20) ||
                    pkt.byteLen % 4 != 0 || pkt.gpuVa % 4 != 0) {
                    LOG_ERROR("GPU: FillMemory bad length/alignment "
                              "(len=%llu, va=0x%llx)",
                              static_cast<unsigned long long>(pkt.byteLen),
                              static_cast<unsigned long long>(pkt.gpuVa));
                    return false;
                }
                u8* host = static_cast<u8*>(addressSpace_->gpuTranslate(pkt.gpuVa));
                if (!host) {
                    LOG_ERROR("GPU: FillMemory at 0x%llx — unmapped",
                              static_cast<unsigned long long>(pkt.gpuVa));
                    return false;
                }
                for (u64 off = 0; off < pkt.byteLen; off += 0x1000) {
                    u64 pa = 0;
                    if (!addressSpace_->gpuTranslateRaw(pkt.gpuVa + off, &pa)) {
                        LOG_ERROR("GPU: FillMemory span unmapped at 0x%llx",
                                  static_cast<unsigned long long>(pkt.gpuVa + off));
                        return false;
                    }
                }
                // Direct-memory pages are pinned for the process lifetime
                // (GUEST_ABI §9), so a concurrent unmap cannot free them.
                u32* p = reinterpret_cast<u32*>(host);
                for (u64 i = 0; i < pkt.byteLen / 4; ++i) p[i] = pkt.pattern;
                break;
            }

            default:
                LOG_ERROR("GPU: unknown opcode %u — aborting buffer",
                          static_cast<u32>(hdr.op));
                return false;
        }

        cursor += hdr.size;
    }

    backend_->frameEnd(readbackHint_); // Vulkan: submit + readback; software: no-op
    return true;
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
extern "C" int32_t psbXGnmSubmitCompute(uint32_t count, void** ccbAddrs,
                                        uint32_t* ccbSizesInBytes) {
    return sceGnmSubmitCompute(count, ccbAddrs, ccbSizesInBytes);
}
extern "C" int32_t psbXGnmWaitQueueIdle(uint32_t queueMask) {
    return sceGnmWaitQueueIdle(queueMask);
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
    for (u32 i = 0; i < count; ++i)
        if (!dcbAddrs[i] || dcbSizesInBytes[i] == 0) return -1;

    if (dev->pipelined()) {
        // Phase-5 pipelined path: validate, snapshot, enqueue, return.
        for (u32 i = 0; i < count; ++i) {
            if (!dev->validateCommandBuffer(dcbAddrs[i], dcbSizesInBytes[i])) {
                LOG_ERROR("GPU: malformed command buffer rejected at submit "
                          "(%u bytes)", dcbSizesInBytes[i]);
                return -1;
            }
        }
        for (u32 i = 0; i < count; ++i) {
            const bool last = (i + 1 == count);
            if (dev->gfxQ_.submit(dcbAddrs[i], dcbSizesInBytes[i], last) != 0)
                return -1;
        }
        dev->checkFrameLimitFromGuest();
        return 0;
    }

    for (u32 i = 0; i < count; ++i) {
        if (!dev->executeItem(dcbAddrs[i], dcbSizesInBytes[i], i + 1 == count))
            return -1;
    }
    dev->checkFrameLimitFromGuest();
    return 0;
}

// ---- Phase 5 compute queue --------------------------------------------------
extern "C" int32_t sceGnmSubmitCompute(uint32_t count, void** ccbAddrs,
                                       uint32_t* ccbSizesInBytes) {
    Device* dev = activeDevice();
    if (!dev) {
        LOG_ERROR("GPU: compute submit with no active device");
        return -1;
    }
    if (!ccbAddrs || !ccbSizesInBytes || count == 0) return -1;
    for (u32 i = 0; i < count; ++i)
        if (!ccbAddrs[i] || ccbSizesInBytes[i] == 0) return -1;

    if (dev->pipelined()) {
        for (u32 i = 0; i < count; ++i) {
            if (!dev->validateCommandBuffer(ccbAddrs[i], ccbSizesInBytes[i])) {
                LOG_ERROR("GPU: malformed compute buffer rejected at submit");
                return -1;
            }
        }
        // Compute submits are not display frames: no completeFrame.
        for (u32 i = 0; i < count; ++i)
            if (dev->computeQ_.submit(ccbAddrs[i], ccbSizesInBytes[i], false) != 0)
                return -1;
        return 0;
    }

    for (u32 i = 0; i < count; ++i)
        if (!dev->executeCommandBuffer(ccbAddrs[i], ccbSizesInBytes[i]))
            return -1;
    return 0;
}

extern "C" int32_t sceGnmWaitQueueIdle(uint32_t queueMask) {
    Device* dev = activeDevice();
    if (!dev) return -1;
    if (queueMask == 0 || queueMask > 3) return -1;
    if (queueMask & kQueueGraphics) dev->gfxQ_.waitIdle();
    if (queueMask & kQueueCompute)  dev->computeQ_.waitIdle();
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
#if defined(_WIN32)
// Guests must reach these through the MASM thunks (w64_thunks.asm) — a
// direct MSVC-compiled binding receives SysV register arguments (audio_tone's
// GNM submit failed silently: count read from RCX instead of RDI).
extern "C" {
void psbTWinGnmSubmit(void);
void psbTWinGnmSubmitCompute(void);
void psbTWinGnmWaitQueueIdle(void);
void psbTWinGnmReserveGpuVa(void);
void psbTWinGnmMapGpuVa(void);
void psbTWinGnmUnmapGpuVa(void);
}
const struct { const char* name; void* fn; } kGpuExports[] = {
    {"sceGnmSubmitCommandBuffers", (void*)&psbTWinGnmSubmit},
    {"sceGnmSubmitCompute",        (void*)&psbTWinGnmSubmitCompute},
    {"sceGnmWaitQueueIdle",        (void*)&psbTWinGnmWaitQueueIdle},
    {"sceGnmReserveGpuAddress",    (void*)&psbTWinGnmReserveGpuVa},
    {"sceGnmMapGpuAddress",        (void*)&psbTWinGnmMapGpuVa},
    {"sceGnmUnmapGpuAddress",      (void*)&psbTWinGnmUnmapGpuVa},
};
#else
const struct { const char* name; void* fn; } kGpuExports[] = {
    {"sceGnmSubmitCommandBuffers", (void*)&sceGnmSubmitCommandBuffers},
    {"sceGnmSubmitCompute",        (void*)&sceGnmSubmitCompute},
    {"sceGnmWaitQueueIdle",        (void*)&sceGnmWaitQueueIdle},
    {"sceGnmReserveGpuAddress",    (void*)&sceGnmReserveGpuAddress},
    {"sceGnmMapGpuAddress",        (void*)&sceGnmMapGpuAddress},
    {"sceGnmUnmapGpuAddress",      (void*)&sceGnmUnmapGpuAddress},
};
#endif
} // namespace

void* findGpuExport(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kGpuExports)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

} // namespace psb::gpu
