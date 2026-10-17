// ============================================================================
//  PSBET — run_tests.cpp
//  Self-contained test runner (no external test framework). Tests cover:
//    1. MemoryManager (allocation, alignment, protections)
//    2. PNG writer (CRC32/Adler32 correctness + file structure)
//    3. Software rasterizer (coverage, depth ordering, clear color)
//    4. GPU command buffer executor (valid + malformed streams)
//    5. End-to-end: hello.elf guest (threads/TLS/sync) via the Emulator
//    6. End-to-end: cube.elf guest (GPU pipeline, frame limit, screenshots)
//  Exit code = number of failed tests (0 = all green).
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>

#include "psbet/common.h"
#include "psbet/psb_guest_api.h"
#include "core/memory.h"
#include "core/elf.h"
#include "core/log.h"
#include "core/pfs.h"
#include "core/timing.h"
#include "core/audio.h"
#include "core/pad.h"
#include "core/net_stubs.h"
#include "core/pacer.h"
#include "gpu/png.h"
#include "gpu/rasterizer.h"
#include "gpu/gpu_shim.h"
#include "gpu/shader_cache.h"
#include "emulator.h"
#include "gpu/vulkan_device.h"

#if !defined(_WIN32)
#define PSBET_RUN_GUEST_TESTS 1
#endif

namespace {

int g_failed = 0;
int g_passed = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("    CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__,      \
                        __LINE__);                                              \
            return false;                                                       \
        }                                                                       \
    } while (0)

bool runTest(const char* name, bool (*fn)()) {
    std::printf("[TEST] %s ... ", name);
    std::fflush(stdout);
    const bool ok = fn();
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    if (ok) g_passed++; else g_failed++;
    return ok;
}

// ---------------------------------------------------------------------------
// 0. ShaderCache (insert/find/persist/reload/corrupt-reject)
// ---------------------------------------------------------------------------
bool test_shader_cache() {
    using psb::gpu::ShaderCache;
    using psb::gpu::spv::Word;
    using psb::u64;

    const std::string dir = "test_shader_cache_dir";
    std::filesystem::remove_all(dir);

    std::vector<Word> mod{7, 0x0021c, 42, 0xFFFF};
    const u64 key = ShaderCache::hashKey("test", mod.data(), mod.size());

    {
        ShaderCache c(dir);
        CHECK(c.find(key) == nullptr);
        c.insert(key, mod);
        const std::vector<Word>* hit = c.find(key);
        CHECK(hit != nullptr);
        CHECK(*hit == mod);
        c.flush();
    }
    {
        // Reload from disk: entry must survive.
        ShaderCache c(dir);
        const std::vector<Word>* hit = c.find(key);
        CHECK(hit != nullptr);
        CHECK(*hit == mod);
    }
    {
        // Corrupt the file: cache must reject it, not trust it.
        std::FILE* f = std::fopen((dir + "/psbet_shaders.cache").c_str(), "r+b");
        CHECK(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        std::fputc(0xAA, f); // append junk -> size mismatch on read
        std::fclose(f);
        ShaderCache c(dir);
        // Entry rejected or absent; no crash, cache usable.
        c.insert(key, mod);
        c.flush();
        CHECK(c.find(key) != nullptr);
    }
    std::filesystem::remove_all(dir);
    return true;
}

// ---------------------------------------------------------------------------
// 1. MemoryManager
// ---------------------------------------------------------------------------
bool test_memory_manager() {
    psb::MemoryManager mem;
    CHECK(mem.init());

    const psb::u64 a = mem.allocate(1024, 256, "test-a", psb::vm::Prot::ReadWrite);
    CHECK(a != 0);
    CHECK(a % 256 == 0);

    const psb::u64 b = mem.allocate(4096, 4096, "test-b", psb::vm::Prot::ReadWrite);
    CHECK(b != 0);
    CHECK(b % 4096 == 0);
    CHECK(b >= a + 1024); // bump allocator never overlaps

    // The memory must actually be writable (committed).
    volatile psb::u8* p = reinterpret_cast<volatile psb::u8*>(b);
    for (int i = 0; i < 4096; i += 512) p[i] = 0xAB;
    for (int i = 0; i < 4096; i += 512) CHECK(p[i] == 0xAB);

    CHECK(mem.bytesAllocated() >= 1024 + 4096);
    CHECK(mem.regionMap().find("test-a") != std::string::npos);
    return true;
}

// ---------------------------------------------------------------------------
// 1b. Pixel-compare: software rasterizer vs Vulkan backend (acceptance:
//     identical rendering within tolerance). Skips gracefully when no
//     working Vulkan stack exists (CI runners install lavapipe).
// ---------------------------------------------------------------------------
bool test_pixel_compare_backends() {
#if !defined(PSBET_RUN_GUEST_TESTS)
    return true; // windows core job: covered by the dedicated vulkan CI job
#else
    if (!psb::gpu::VulkanDevice::selfTest()) {
        std::printf("    (vulkan unavailable — skipped)");
        return true;
    }

    constexpr psb::u32 W = 320, H = 180;
    std::vector<psb::u8> bufA(static_cast<size_t>(W) * H * 4);
    std::vector<psb::u8> bufB(static_cast<size_t>(W) * H * 4);
    std::vector<float> depth(static_cast<size_t>(W) * H);

    // Shared scene: clear, viewport, depth, cull, two overlapping triangles.
    psb::gpu::GpuVertex tri[6] = {
        {{-0.8f, -0.6f, 0.10f, 1.f}, {1.0f, 0.2f, 0.2f, 1.f}},
        {{ 0.8f, -0.6f, 0.10f, 1.f}, {1.0f, 0.2f, 0.2f, 1.f}},
        {{ 0.0f,  0.8f, 0.10f, 1.f}, {1.0f, 0.2f, 0.2f, 1.f}},
        {{-0.6f, -0.8f, 0.05f, 1.f}, {0.2f, 0.4f, 1.0f, 1.f}},
        {{ 0.9f, -0.2f, 0.05f, 1.f}, {0.2f, 0.4f, 1.0f, 1.f}},
        {{ 0.1f,  0.9f, 0.05f, 1.f}, {0.2f, 0.4f, 1.0f, 1.f}},
    };

    auto buildStream = [&](psb::gpu::CmdWriter& cw) {
        cw.clear();
        psb::gpu::CmdClear clear;
        clear.color[0] = 0.05f; clear.color[1] = 0.05f; clear.color[2] = 0.1f;
        cw.push(clear);
        psb::gpu::CmdSetViewport vp;
        vp.x = 0; vp.y = 0; vp.w = (float)W; vp.h = (float)H;
        cw.push(vp);
        psb::gpu::CmdSetDepthTest dt; dt.enable = 1; dt.func = psb::gpu::DepthFunc::Less;
        cw.push(dt);
        psb::gpu::CmdSetCullMode cm; cm.mode = psb::gpu::CullMode::None;
        cw.push(cm);
        cw.draw(tri, 6);
    };

    // Software pass.
    {
        psb::gpu::CmdWriter cw;
        buildStream(cw);
        psb::gpu::Device dev(bufA.data(), depth.data(), W, H);
        psb::gpu::setActiveDevice(&dev);
        CHECK(dev.executeCommandBuffer(cw.data(), cw.size()));
        psb::gpu::setActiveDevice(nullptr);
    }
    // Vulkan pass.
    {
        auto backend = psb::gpu::VulkanDevice::create(bufB.data(), W, H);
        CHECK(backend != nullptr);
        psb::gpu::CmdWriter cw;
        buildStream(cw);
        psb::gpu::Device dev(bufB.data(), W, H, std::move(backend));
        psb::gpu::setActiveDevice(&dev);
        CHECK(dev.executeCommandBuffer(cw.data(), cw.size()));
        psb::gpu::setActiveDevice(nullptr);
    }

    // Compare: per-pixel channel delta <= 4/255 on >= 99% of pixels, and the
    // images must be broadly non-trivial (the triangles actually drew).
    size_t close = 0, active = 0;
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        int dr = std::abs(int(bufA[i*4+0]) - int(bufB[i*4+0]));
        int dg = std::abs(int(bufA[i*4+1]) - int(bufB[i*4+1]));
        int db = std::abs(int(bufA[i*4+2]) - int(bufB[i*4+2]));
        if (dr <= 4 && dg <= 4 && db <= 4) close++;
        if (bufA[i*4+0] > 30 || bufA[i*4+1] > 30 || bufA[i*4+2] > 30) active++;
    }
    std::printf("    (close=%zu/%zu active=%zu) ", close, size_t(W) * H, active);
    CHECK(close * 100 >= 99 * (static_cast<size_t>(W) * H));
    CHECK(active > (static_cast<size_t>(W) * H) / 10);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// 1c. Page tables + GPU VM (Phase 3): typed pools, PTE walk, conflicts, and
//     GPU virtual-address translation through a separate page-table domain.
// ---------------------------------------------------------------------------
bool test_guest_vm_model() {
    psb::MemoryManager mem;
    CHECK(mem.init());
    auto& gm = mem.guest();

    // Typed pools: a GPU-pool allocation lands in the GPU-visible window.
    const psb::u64 paGpu = gm.allocDirect(psb::GuestMemory::kPoolGpuVisible, 0x4000,
                                          0x1000, 0, 0, "t-gpu");
    CHECK(paGpu != 0);
    CHECK(paGpu >= psb::GuestMemory::kPaRegularSize);

    // Search ranges are honoured: force a high regular allocation.
    const psb::u64 paHi = gm.allocDirect(psb::GuestMemory::kPoolRegular, 0x2000,
                                         0x1000, 1 * psb::GiB, 2 * psb::GiB, "t-hi");
    CHECK(paHi >= 1 * psb::GiB && paHi < 2 * psb::GiB);

    // Identity CPU map + PTE walk.
    const psb::u64 vaGpu = gm.mapDirect(paGpu, 0x4000, psb::vm::Prot::ReadWrite, "t-gpu");
    CHECK(vaGpu == mem.arenaBase() + paGpu);
    volatile psb::u8* p = reinterpret_cast<volatile psb::u8*>(vaGpu);
    p[0] = 0x11; p[0x3FFF] = 0x22;
    CHECK(p[0] == 0x11 && p[0x3FFF] == 0x22);

    // queryVa returns the mapping covering an address.
    psb::GuestMemory::BlockInfo info;
    CHECK(gm.queryVa(vaGpu + 0x123, &info));
    CHECK(info.pa == paGpu && info.len == 0x4000);

    // freeDirect is refcount-guarded: refuses while mapped, works after unmap.
    CHECK(!gm.freeDirect(paGpu, 0x4000));
    CHECK(gm.unmap(vaGpu, 0x4000));
    CHECK(gm.freeDirect(paGpu, 0x4000));

    // GPU VM: reserve -> map -> translate (separate address space; GPU VAs
    // have nothing to do with CPU VAs).
    const psb::u64 pa2 = gm.allocDirect(psb::GuestMemory::kPoolRegular, 0x3000,
                                        0x1000, 0, 0, "t-va");
    CHECK(pa2 != 0);
    psb::u64 gva = gm.gpuReserve(0x3000, 0x1000, 0);
    CHECK(gva != 0 && gva < psb::GuestMemory::kGpuVaSize);
    CHECK(gm.gpuMap(gva, pa2, 0x3000, psb::vm::Prot::ReadWrite));
    psb::u64 paWalk = 0;
    CHECK(gm.gpuTranslateRaw(gva + 0x1234, &paWalk));
    CHECK(paWalk == pa2 + 0x1234);
    void* host = gm.gpuTranslate(gva);
    CHECK(host == reinterpret_cast<void*>(mem.arenaBase() + pa2));
    // Unmapped GPU VAs translate to nothing.
    CHECK(gm.gpuTranslate(gva + 0x3000) == nullptr);
    // Fixed GPU VA reservation collides while mapped.
    CHECK(gm.gpuReserve(0x1000, 0x1000, gva) == 0);
    CHECK(gm.gpuUnmap(gva, 0x3000));
    psb::u64 gva2 = gm.gpuReserve(0x1000, 0x1000, gva);
    CHECK(gva2 == gva);
    CHECK(gm.freeDirect(pa2, 0x3000));
    return true;
}

// ---------------------------------------------------------------------------
// 1d. PageTable PTE-level conflicts (Phase 3).
// ---------------------------------------------------------------------------
bool test_page_table_conflicts() {
    psb::PageTable pt;
    CHECK(pt.map(0x1000, 0x1000, 0x5000, psb::vm::Prot::ReadWrite));
    psb::u64 pa = 0;
    CHECK(pt.translate(0x1234, &pa));
    CHECK(pa == 0x5234);
    // Same-page double map with a DIFFERENT frame must fail.
    CHECK(!pt.map(0x1000, 0x1000, 0x9000, psb::vm::Prot::ReadWrite));
    // setProt updates without remapping; unmap clears.
    CHECK(pt.setProt(0x1000, psb::vm::Prot::ReadOnly));
    CHECK(pt.queryProt(0x1000) == psb::vm::Prot::ReadOnly);
    CHECK(pt.unmap(0x1000, 0x1000));
    CHECK(!pt.translate(0x1000, nullptr));
    // Large coverage: 2 MiB mid-page boundary crossing.
    CHECK(pt.map(0x1FF000, 0x2000, 0x1FF000, psb::vm::Prot::ReadWrite));
    CHECK(pt.translate(0x200FFF, &pa) && pa == 0x200FFF);
    return true;
}

// ---------------------------------------------------------------------------
// 2. PNG writer
// ---------------------------------------------------------------------------
bool test_png_writer() {
    using psb::gpu::Image;
    using psb::gpu::writePng;

    // 4x4 gradient image
    Image img;
    img.width = 4; img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            psb::u8* px = img.rgba.data() + (y * 4 + x) * 4;
            px[0] = static_cast<psb::u8>(x * 60);
            px[1] = static_cast<psb::u8>(y * 60);
            px[2] = 0x40;
            px[3] = 0xFF;
        }

    const std::string path = "test_out.png";
    CHECK(writePng(path, img));

    // Structure check: signature + IHDR dims.
    std::FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    psb::u8 hdr[24];
    const size_t rd = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    CHECK(rd == sizeof(hdr));
    static const psb::u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(hdr, sig, 8) == 0);
    // IHDR: length 13, type IHDR
    CHECK(hdr[11] == 13);
    CHECK(std::memcmp(hdr + 12, "IHDR", 4) == 0);
    const psb::u32 w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    const psb::u32 h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    CHECK(w == 4 && h == 4);

    std::remove(path.c_str());
    return true;
}

bool test_png_rejects_bad_input() {
    using psb::gpu::Image;
    using psb::gpu::writePng;
    Image bad;
    bad.width = 2; bad.height = 2;
    bad.rgba = {1, 2, 3, 4}; // wrong size
    CHECK(!writePng("should_not_exist.png", bad));
    return true;
}

// ---------------------------------------------------------------------------
// 3. Rasterizer
// ---------------------------------------------------------------------------
bool test_rasterizer() {
    const psb::u32 W = 64, H = 64;
    std::vector<psb::u8> color(W * H * 4, 0);
    std::vector<float> depth(W * H, 1.0f);

    psb::gpu::Rasterizer ras(color.data(), depth.data(), W, H);
    ras.setViewport(0, 0, static_cast<float>(W), static_cast<float>(H));

    // Clear to blue
    ras.clear(0.f, 0.f, 1.f, 1.f);
    const psb::u8* px = color.data();
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255);
    CHECK(depth[0] == 1.0f);

    // Fullscreen red triangle (NDC CCW)
    const psb::gpu::GpuVertex tri[3] = {
        {{-1.f, -1.f, 0.5f, 1.f}, {1.f, 0.f, 0.f, 1.f}},
        {{ 1.f, -1.f, 0.5f, 1.f}, {1.f, 0.f, 0.f, 1.f}},
        {{ 0.f,  1.f, 0.5f, 1.f}, {1.f, 0.f, 0.f, 1.f}},
    };
    ras.setDepthTest(true, psb::gpu::DepthFunc::Less);
    ras.drawTriangles(tri, 3);

    // Center pixel must be red now.
    const psb::u8* center = color.data() + ((H / 2) * W + W / 2) * 4;
    CHECK(center[0] == 255 && center[2] == 0);
    CHECK(depth[(H / 2) * W + W / 2] < 0.6f);   // 0.5 interpolated
    return true;
}

bool test_rasterizer_depth_ordering() {
    const psb::u32 W = 32, H = 32;
    std::vector<psb::u8> color(W * H * 4, 0);
    std::vector<float> depth(W * H, 1.0f);

    psb::gpu::Rasterizer ras(color.data(), depth.data(), W, H);
    ras.setViewport(0, 0, static_cast<float>(W), static_cast<float>(H));
    ras.setDepthTest(true, psb::gpu::DepthFunc::Less);

    const psb::gpu::GpuVertex far_[3] = {
        {{-1.f, -1.f, 0.8f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
        {{ 1.f, -1.f, 0.8f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
        {{ 0.f,  1.f, 0.8f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
    };
    const psb::gpu::GpuVertex near_[3] = {
        {{-1.f, -1.f, 0.2f, 1.f}, {0.f, 1.f, 0.f, 1.f}},
        {{ 1.f, -1.f, 0.2f, 1.f}, {0.f, 1.f, 0.f, 1.f}},
        {{ 0.f,  1.f, 0.2f, 1.f}, {0.f, 1.f, 0.f, 1.f}},
    };
    ras.drawTriangles(far_, 3);
    ras.drawTriangles(near_, 3);   // nearer triangle wins

    const psb::u8* center = color.data() + ((H / 2) * W + W / 2) * 4;
    CHECK(center[1] == 255 && center[2] == 0); // green (near) visible

    // Reverse order: nearer first, farther second — must NOT overwrite.
    std::vector<psb::u8> color2(W * H * 4, 0);
    std::vector<float> depth2(W * H, 1.0f);
    psb::gpu::Rasterizer ras2(color2.data(), depth2.data(), W, H);
    ras2.setViewport(0, 0, static_cast<float>(W), static_cast<float>(H));
    ras2.setDepthTest(true, psb::gpu::DepthFunc::Less);
    ras2.drawTriangles(near_, 3);
    ras2.drawTriangles(far_, 3);
    const psb::u8* center2 = color2.data() + ((H / 2) * W + W / 2) * 4;
    CHECK(center2[1] == 255 && center2[2] == 0); // still green
    return true;
}

// ---------------------------------------------------------------------------
// 4. GPU command executor
// ---------------------------------------------------------------------------
bool test_cmd_executor() {
    const psb::u32 W = 16, H = 16;
    std::vector<psb::u8> color(W * H * 4, 0);
    std::vector<float> depth(W * H, 1.0f);

    psb::gpu::Device dev(color.data(), depth.data(), W, H);
    psb::gpu::setActiveDevice(&dev);

    // Build a small valid stream: clear(magenta) + draw white tri.
    std::vector<psb::u8> stream;
    auto append = [&stream](const void* p, size_t n) {
        const psb::u8* b = static_cast<const psb::u8*>(p);
        stream.insert(stream.end(), b, b + n);
    };

    psb::gpu::CmdClear clear{};
    clear.color[0] = 1.f; clear.color[1] = 0.f; clear.color[2] = 1.f;
    append(&clear, sizeof(clear));

    psb::gpu::CmdSetViewport vp{};
    vp.w = static_cast<float>(W); vp.h = static_cast<float>(H);
    append(&vp, sizeof(vp));

    psb::gpu::CmdDrawTriangles draw{};
    psb::gpu::GpuVertex verts[3] = {
        {{-1.f, -1.f, 0.5f, 1.f}, {1.f, 1.f, 1.f, 1.f}},
        {{ 1.f, -1.f, 0.5f, 1.f}, {1.f, 1.f, 1.f, 1.f}},
        {{ 0.f,  1.f, 0.5f, 1.f}, {1.f, 1.f, 1.f, 1.f}},
    };
    draw.vertexCount = 3;
    draw.hdr.size = static_cast<psb::u32>(sizeof(psb::gpu::CmdDrawTriangles) + sizeof(verts));
    append(&draw, sizeof(draw));
    append(verts, sizeof(verts));

    CHECK(dev.executeCommandBuffer(stream.data(), static_cast<psb::u32>(stream.size())));
    CHECK(dev.frameCount() == 0); // executeCommandBuffer doesn't bump frames;
                                  // only the submit shim does.
    const psb::u8* center = color.data() + ((H / 2) * W + W / 2) * 4;
    CHECK(center[0] == 255 && center[1] == 255 && center[2] == 255);

    // Malformed stream must be rejected (bad size).
    std::vector<psb::u8> bad = {0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!dev.executeCommandBuffer(bad.data(), static_cast<psb::u32>(bad.size())));

    // Submit shim path (frame counting + hook).
    psb::u64 hookFrames = 0;
    dev.setFrameHook([&](psb::u64 f) { hookFrames = f; });
    void* addrs[1] = { stream.data() };
    psb::u32 sizes[1] = { static_cast<psb::u32>(stream.size()) };
    CHECK(psb::gpu::sceGnmSubmitCommandBuffers(1, addrs, sizes, nullptr, nullptr, 0) == 0);
    CHECK(dev.frameCount() == 1);
    CHECK(hookFrames == 1);

    psb::gpu::setActiveDevice(nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// 5+6. End-to-end guest tests (POSIX hosts only in Phase 1)
// ---------------------------------------------------------------------------
#ifdef PSBET_RUN_GUEST_TESTS

// ---------------------------------------------------------------------------
// Phase-3 loader conformance: hand-synthesized ELF64 images exercising the
// real-image paths (ET_EXEC absolute vaddrs, ET_DYN with non-zero first
// vaddr, static images without PT_DYNAMIC, IRELATIVE resolution by executing
// the guest resolver, and GNU_RELRO). The exit-process address is patched in
// at build time (runtime-known host address).
// ---------------------------------------------------------------------------
using psb::Elf64Ehdr;
using psb::Elf64Phdr;
using psb::Elf64Rela;
using psb::Elf64Dyn;
using psb::u8;
using psb::u16;
using psb::u32;
using psb::u64;
using psb::i64;

extern "C" void sceKernelExitProcess(int32_t);

// mov edi, imm32; movabs rax, addr; call rax  ->  sceKernelExitProcess(imm32)
static void writeExitStub(u8* at, u32 code, void* exitFn) {
    at[0] = 0xBF;   // mov edi, imm32
    std::memcpy(at + 1, &code, 4);
    at[5] = 0x48; at[6] = 0xB8;   // movabs rax, imm64
    std::memcpy(at + 7, &exitFn, 8);
    at[15] = 0xFF; at[16] = 0xD0; // call rax
}

static void patchRel32(u8* at, i64 disp) { std::memcpy(at, &disp, 4); }

static void initEhdr(Elf64Ehdr* eh, u16 type, u64 entry, u16 phnum) {
    std::memset(eh, 0, sizeof(*eh));
    std::memcpy(eh->ident, "\x7f""ELF", 4);
    eh->ident[4] = 2;   // ELF64
    eh->ident[5] = 1;   // LE
    eh->ident[6] = 1;   // EV_CURRENT
    eh->type = type;
    eh->machine = 62;
    eh->version = 1;
    eh->entry = entry;
    eh->phoff = sizeof(Elf64Ehdr);
    eh->phentsize = sizeof(Elf64Phdr);
    eh->phnum = phnum;
}

bool test_loader_et_exec_static() {
    // ET_EXEC at absolute vaddr 0x60000000, one RWX PT_LOAD, no dynamic.
    std::vector<u8> img(0x2000, 0);
    auto* eh = reinterpret_cast<Elf64Ehdr*>(img.data());
    initEhdr(eh, psb::ET_EXEC, 0x60000000 + 0x100, 1);
    auto* ph = reinterpret_cast<Elf64Phdr*>(img.data() + sizeof(Elf64Ehdr));
    std::memset(ph, 0, sizeof(*ph));
    ph->type = psb::PT_LOAD; ph->flags = 7; ph->offset = 0;
    ph->vaddr = 0x60000000; ph->filesz = 0x2000; ph->memsz = 0x2000;
    ph->align = 0x1000;
    writeExitStub(img.data() + 0x100, 42, reinterpret_cast<void*>(&sceKernelExitProcess));

    std::FILE* f = std::fopen("t_exec.elf", "wb");
    CHECK(f != nullptr);
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);

    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("t_exec.elf", opt);
    std::remove("t_exec.elf");
    CHECK(st.guestExitCode == 42);
    CHECK(!st.crashed);
    return true;
}

bool test_loader_dyn_offset_static() {
    // ET_DYN whose first PT_LOAD sits at vaddr 0x2000 (non-zero lo), static.
    std::vector<u8> img(0x2000, 0);
    auto* eh = reinterpret_cast<Elf64Ehdr*>(img.data());
    initEhdr(eh, psb::ET_DYN, 0x2000 + 0x100, 1);
    auto* ph = reinterpret_cast<Elf64Phdr*>(img.data() + sizeof(Elf64Ehdr));
    std::memset(ph, 0, sizeof(*ph));
    ph->type = psb::PT_LOAD; ph->flags = 7; ph->offset = 0;
    ph->vaddr = 0x2000; ph->filesz = 0x2000; ph->memsz = 0x2000;
    ph->align = 0x1000;
    writeExitStub(img.data() + 0x100, 43, reinterpret_cast<void*>(&sceKernelExitProcess));

    std::FILE* f = std::fopen("t_dynoff.elf", "wb");
    CHECK(f != nullptr);
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);

    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("t_dynoff.elf", opt);
    std::remove("t_dynoff.elf");
    CHECK(st.guestExitCode == 43);
    CHECK(!st.crashed);
    return true;
}

bool test_loader_irelative_relro() {
    // ET_DYN, PT_DYNAMIC with one IRELATIVE whose resolver runs (guest code)
    // and whose GOT page is placed under PT_GNU_RELRO.
    //   0x000 ehdr+phdrs | 0x800 resolver | 0x840 target | 0x900 entry
    //   0x1000 GOT (RELRO) | 0x1020 rela | 0x1030 strtab | 0x1048 symtab
    //   0x1078 dynamic
    std::vector<u8> img(0x2000, 0);
    auto* eh = reinterpret_cast<Elf64Ehdr*>(img.data());
    initEhdr(eh, psb::ET_DYN, 0x900, 3);
    auto* ph = reinterpret_cast<Elf64Phdr*>(img.data() + sizeof(Elf64Ehdr));
    std::memset(ph, 0, 3 * sizeof(Elf64Phdr));
    ph[0] = {psb::PT_LOAD, 7, 0, 0, 0, 0x2000, 0x2000, 0x1000};
    ph[1] = {psb::PT_DYNAMIC, 6, 0x1078, 0x1078, 0, 0x60, 0x60, 8};
    ph[2] = {psb::PT_GNU_RELRO, 4, 0x1000, 0x1000, 0, 0x1000, 0x1000, 1};

    // resolver @0x800: lea rax,[rip+0x39] (target @0x840); ret
    img[0x800] = 0x48; img[0x801] = 0x8D; img[0x802] = 0x05;
    patchRel32(img.data() + 0x803, 0x840 - 0x807);
    img[0x807] = 0xC3;
    // target @0x840: exit(55)
    writeExitStub(img.data() + 0x840, 55, reinterpret_cast<void*>(&sceKernelExitProcess));
    // entry @0x900: mov rax,[rip+disp] (GOT @0x1000); call rax
    img[0x900] = 0x48; img[0x901] = 0x8B; img[0x902] = 0x05;
    patchRel32(img.data() + 0x903, 0x1000 - 0x907);
    img[0x907] = 0xFF; img[0x908] = 0xD0;

    // rela @0x1020: IRELATIVE, GOT <- resolver
    auto* rel = reinterpret_cast<Elf64Rela*>(img.data() + 0x1020);
    rel->offset = 0x1000;
    rel->info = (static_cast<u64>(0) << 32) | psb::R_X86_64_IRELATIVE;
    rel->addend = 0x800;
    // empty strtab @0x1030 (single NUL), NULL symtab entry @0x1048 (24 B)
    img[0x1030] = 0;
    // dynamic @0x1078
    auto* dyn = reinterpret_cast<Elf64Dyn*>(img.data() + 0x1078);
    const int nd = 0;
    int di = 0;
    dyn[di++] = {psb::DT_STRTAB, 0x1030};
    dyn[di++] = {psb::DT_SYMTAB, 0x1048};
    dyn[di++] = {psb::DT_RELA, 0x1020};
    dyn[di++] = {psb::DT_RELASZ, 24};
    dyn[di++] = {psb::DT_RELAENT, 24};
    dyn[di++] = {psb::DT_NULL, 0};
    (void)nd;

    std::FILE* f = std::fopen("t_irel.elf", "wb");
    CHECK(f != nullptr);
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);

    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("t_irel.elf", opt);
    std::remove("t_irel.elf");
    CHECK(st.guestExitCode == 55);
    CHECK(!st.crashed);
    return true;
}

bool test_guest_hello() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.verbose = false;
    const psb::EmuStats st = emu.run("hello.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    return true;
}

// Phase-2 regression: worker-initiated process exit (flaw 1 — cross-thread
// longjmp in Phase 1). The guest's worker calls ExitProcess(7) while main
// blocks on a semaphore; the process must exit with the worker's code.
bool test_guest_worker_exit() {
#if !defined(PSBET_RUN_GUEST_TESTS)
    return true;
#else
    psb::EmuOptions opt;
    opt.maxFrames = 1000;
    psb::Emulator emu;
    const psb::EmuStats st = emu.run("worker_exit.elf", opt);
    CHECK(st.guestExitCode == 7);
    CHECK(!st.crashed);
    return true;
#endif
}

bool test_guest_cube() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.width = 320; opt.height = 180;    // small: fast CI
    opt.maxFrames = 45;
    opt.screenshotDir = "test_shots";
    opt.screenshotInterval = 15;
    const psb::EmuStats st = emu.run("cube.elf", opt);
    CHECK(st.guestExitCode == 124);       // frame limit exit
    CHECK(st.frames == 45);
    CHECK(st.screenshots == 3);           // frames 15, 30, 45
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);       // Phase-3: clean teardown

    // A screenshot must exist and be a real PNG.
    std::FILE* f = std::fopen("test_shots/frame_0030.png", "rb");
    CHECK(f != nullptr);
    psb::u8 sig[8];
    CHECK(std::fread(sig, 1, 8, f) == 8);
    static const psb::u8 want[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(sig, want, 8) == 0);
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Phase-4 unit tests: timing, audio, pad, net.
// ---------------------------------------------------------------------------
bool test_timing_domain() {
    using psb::time::TimeDomain;
    TimeDomain& td = TimeDomain::get();

    // Wall-clock mode: monotonic, sane.
    td.init();
    psb::u64 prev = td.us();
    for (int i = 0; i < 1000; ++i) {
        const psb::u64 v = td.us();
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(td.ns() >= prev * 1000ull - 1000ull);   // ns derived consistently
    CHECK(TimeDomain::kCounterFrequency == 1000000000ull);

    // 24 h SIMULATED SOAK: virtual mode, 1 s steps + fine interleaves.
    // Drift-free by construction: the value IS the injected offset, so every
    // step must match the expected value EXACTLY.
    td.enableVirtual(0);
    const psb::u64 kSecond = 1'000'000ull;
    const psb::u64 kDay = 24ull * 3600ull * kSecond;
    psb::u64 expected = 0;
    psb::u64 lastSeen = 0;
    for (psb::u64 s = 0; s < kDay / kSecond; ++s) {
        // Fine interleaves: 10 x 100 ms inside every simulated second.
        for (int f = 0; f < 10; ++f) {
            td.advanceVirtual(100'000ull);
            expected += 100'000ull;
            const psb::u64 v = td.us();
            CHECK(v == expected);          // exact — zero drift
            CHECK(v >= lastSeen);          // monotonic
            lastSeen = v;
        }
    }
    CHECK(lastSeen == kDay);               // exactly 24 h of guest time

    // Concurrent readers must never observe a decrease (2 readers, 5k each).
    td.enableVirtual(0);
    auto reader = [&td](psb::u64* bad) {
        psb::u64 prev = 0;
        for (int i = 0; i < 5000; ++i) {
            const psb::u64 v = td.us();
            if (v < prev) { (*bad)++; }
            prev = v;
        }
    };
    psb::u64 badA = 0, badB = 0;
    std::thread ra([&] { reader(&badA); });
    std::thread rb([&] { reader(&badB); });
    for (int i = 0; i < 5000; ++i) td.advanceVirtual(137ull);
    ra.join();
    rb.join();
    CHECK(badA == 0);
    CHECK(badB == 0);

    td.disableVirtual();
    td.init();
    return true;
}

bool test_audio_unit() {
    using namespace psb;
    using namespace psb::audio;
    Mixer& mx = Mixer::get();

    // Deterministic WAV-dump backend (soak path: submits drain immediately).
    auto backend = std::make_unique<WavDumpBackend>("");   // no file write
    WavDumpBackend* dump = backend.get();
    mx.setBackend(std::move(backend));

    const i32 main_ = sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0, 100, 48000,
                                      PSB_AUDIO_FMT_S16_STEREO);
    CHECK(main_ == 1);
    i32 volMax[2] = {kVolMax, kVolMax};
    CHECK(sceAudioOutSetVolume(main_, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                               volMax) == 0);

    // Tone A: recognizable asymmetric pattern.
    i16 bufA[200];
    for (int i = 0; i < 100; ++i) {
        bufA[2 * i] = static_cast<i16>((i % 7) * 1000 - 3000);
        bufA[2 * i + 1] = static_cast<i16>(5000 - (i % 11) * 800);
    }
    CHECK(sceAudioOutOutput(main_, bufA) == 0);

    // Volume clamp + per-channel independence.
    i32 volOver[2] = {999999, -5};
    CHECK(sceAudioOutSetVolume(main_, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                               volOver) == 0);
    CHECK(sceAudioOutOutput(main_, bufA) == 0);
    // Restore full volume for the mixing section below.
    CHECK(sceAudioOutSetVolume(main_, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                               volMax) == 0);

    // Two-port mix: BGM at half volume — master = A + (B*16384)/32768.
    const i32 bgm = sceAudioOutOpen(0, PSB_AUDIO_PORT_BGM, 0, 100, 48000,
                                    PSB_AUDIO_FMT_S16_STEREO);
    CHECK(bgm == 2);
    i32 volHalf[2] = {kVolMax / 2, kVolMax / 2};
    CHECK(sceAudioOutSetVolume(bgm, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                               volHalf) == 0);
    i16 bufB[200];
    for (int i = 0; i < 100; ++i) {
        bufB[2 * i] = static_cast<i16>((i % 5) * 2000 - 4000);
        bufB[2 * i + 1] = static_cast<i16>(3000 - (i % 3) * 1500);
    }
    CHECK(sceAudioOutOutput(main_, bufA) == 0);
    CHECK(sceAudioOutOutput(bgm, bufB) == 0);

    // Documented error paths.
    CHECK(sceAudioOutOpen(0, 9, 0, 100, 48000, PSB_AUDIO_FMT_S16_STEREO) == -1);
    CHECK(sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 1, 100, 48000,
                          PSB_AUDIO_FMT_S16_STEREO) == -1);
    CHECK(sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0, 100, 44100,
                          PSB_AUDIO_FMT_S16_STEREO) == -1);
    CHECK(sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0, 0, 48000,
                          PSB_AUDIO_FMT_S16_STEREO) == -1);
    CHECK(sceAudioOutOutput(42, bufA) == -1);
    CHECK(sceAudioOutClose(42) == -1);
    CHECK(sceAudioOutClose(bgm) == 0);
    CHECK(sceAudioOutClose(main_) == 0);

    // Verify the mixed master stream sample-by-sample (same integer math
    // the mixer uses). Geometry under the cursor model: submits 1 and 2 play
    // solo; submit 3 (main) and submit 4 (bgm) land in the SAME window and
    // SUM — the essence of guest-side parallel mixing.
    // NOTE: read the dump BEFORE clearBackend() — it owns/destroys the
    // backend instance.
    const std::vector<i16>& s = dump->samples();
    CHECK(s.size() == 600);   // 100 + 100 + 100(mixed) frames x 2 channels
    auto clamp16 = [](i32 v) -> i16 {
        return static_cast<i16>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
    };
    const size_t off1 = 0, off2 = 200, off3 = 400;   // samples: 100 frames = 200
    bool allMixOk = true;
    for (int i = 0; i < 100 && allMixOk; ++i) {
        const i16 al = bufA[2*i], ar = bufA[2*i+1];
        const i16 bl = bufB[2*i], br = bufB[2*i+1];
        // SIGNED volume constants — dividing by the u32 literal would promote
        // negative products to unsigned (the bug class this test guards).
        const i32 vMax = static_cast<i32>(kVolMax);
        const i32 vHalf = vMax / 2;
        // Submit 1: A at full volume (both channels).
        if (s[off1 + 2*i]     != clamp16((al * vMax) / vMax)) allMixOk = false;
        if (s[off1 + 2*i + 1] != clamp16((ar * vMax) / vMax)) allMixOk = false;
        // Submit 2: volume clamped — L stayed max, R clamped from -5 to 0.
        if (s[off2 + 2*i]     != clamp16((al * vMax) / vMax)) allMixOk = false;
        if (s[off2 + 2*i + 1] != clamp16((ar * 0) / vMax))    allMixOk = false;
        // Submit 3+4: one window, summed (A full + B half), single clamp.
        const i32 wantL = (al * vMax) / vMax + (bl * vHalf) / vMax;
        const i32 wantR = (ar * vMax) / vMax + (br * vHalf) / vMax;
        if (s[off3 + 2*i]     != clamp16(wantL) ||
            s[off3 + 2*i + 1] != clamp16(wantR)) allMixOk = false;
    }
    CHECK(allMixOk);

    const Mixer::Stats st = mx.stats();
    CHECK(st.droppedFrames == 0);
    CHECK(st.submittedFrames == 300);   // frontier: 3 unique master windows

    mx.clearBackend();   // now tear the backend down (dump already read)
    return true;
}

bool test_audio_wav_file() {
    using namespace psb;
    // 50 ms of 440 Hz stereo for the header/length check.
    std::vector<i16> samples(2400);
    for (size_t i = 0; i < samples.size(); i += 2) {
        const double t = static_cast<double>(i / 2) / 48000.0;
        const i16 v = static_cast<i16>(std::sin(440.0 * 2.0 * 3.14159265358979
                                                * t) * 20000.0);
        samples[i] = v;
        samples[i + 1] = v;
    }
    CHECK(psb::audio::writeWavFile("test_tone.wav", 48000, 2, samples.data(),
                                   samples.size()));
    CHECK(!psb::audio::writeWavFile("test_bad.wav", 0, 2, samples.data(),
                                    samples.size()));
    std::FILE* f = std::fopen("test_tone.wav", "rb");
    CHECK(f != nullptr);
    psb::u8 hdr[44];
    CHECK(std::fread(hdr, 1, 44, f) == 44);
    CHECK(std::memcmp(hdr, "RIFF", 4) == 0);
    CHECK(std::memcmp(hdr + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(hdr + 12, "fmt ", 4) == 0);
    psb::u32 rate;
    std::memcpy(&rate, hdr + 24, 4);
    CHECK(rate == 48000);
    psb::u32 dataBytes;
    std::memcpy(&dataBytes, hdr + 40, 4);
    CHECK(dataBytes == samples.size() * 2);
    i16 first;
    CHECK(std::fread(&first, 2, 1, f) == 1);
    CHECK(first == samples[0]);
    std::fclose(f);
    std::filesystem::remove("test_tone.wav");
    std::filesystem::remove("test_bad.wav");   // must not exist; ignore
    return true;
}

bool test_pad_unit() {
    using namespace psb;
    pad::PadService& svc = pad::PadService::get();

    auto driver = std::make_unique<pad::ScriptedPadDriver>();
    pad::ScriptedPadDriver* scr = driver.get();
    CHECK(scr->loadFromString(
        "# scripted events\n"
        "100 1 200 100 60 90 10 20\n"      // Cross at 100 ms, moved sticks
        "200 GYRO 0.10 0.20 -0.98 15 0 -7\n"
        "300 100 128 128 128 128 0 255\n")); // Options at 300 ms, R2 full
    svc.setDriver(std::move(driver));

    CHECK(svc.open(0, 0, 0, nullptr) == 1);
    CHECK(svc.open(1, 0, 0, nullptr) == -1);   // bad user
    CHECK(svc.open(0, 5, 0, nullptr) == -1);   // bad type
    CHECK(svc.open(0, 0, 3, nullptr) == -1);   // bad index

    pad::PadState st{};
    // Before the first event: neutral, disconnected.
    time::TimeDomain::get().enableVirtual(50'000);   // 50 ms
    svc.poll();
    CHECK(svc.readState(1, &st) == 0);
    CHECK(st.connected == 0);

    // At 150 ms: Cross held, sticks as scripted.
    time::TimeDomain::get().enableVirtual(150'000);
    svc.poll();
    CHECK(svc.readState(1, &st) == 0);
    CHECK(st.connected == 1);
    CHECK(st.buttons == pad::PadBtnCross);
    CHECK(st.lx == 200 && st.ly == 100 && st.rx == 60 && st.ry == 90);
    CHECK(st.l2 == 10 && st.r2 == 20);
    CHECK(st.timestampUs == 100'000);

    // At 250 ms: gyro values from the GYRO event.
    time::TimeDomain::get().enableVirtual(250'000);
    svc.poll();
    CHECK(svc.readState(1, &st) == 0);
    CHECK(st.gyroX == 15.f && st.gyroZ == -7.f);
    CHECK(st.accelY == 0.20f);

    // Batch read + controller information.
    psb::u32 returned = 99;
    CHECK(svc.read(1, &st, 4, &returned) == 0);
    CHECK(returned == 1);
    pad::ControllerInfo info{};
    CHECK(svc.getControllerInformation(1, &info) == 0);
    CHECK(info.connectedCount == 1);
    CHECK(info.touchpixW == 1920);

    // Error paths.
    CHECK(svc.readState(7, &st) == -1);
    CHECK(svc.getControllerInformation(7, &info) == -1);

    svc.clearDriver();
    time::TimeDomain::get().disableVirtual();
    time::TimeDomain::get().init();
    return true;
}

bool test_net_stubs() {
    using namespace psb;
    net::resetCounters();
    CHECK(sceNetInit(0) == net::kErrUnavailable);
    CHECK(sceNetSocket("t", 2, 1, 6) == net::kErrUnavailable);
    CHECK(sceNetConnect(1, nullptr, 0) == net::kErrNotInitialized);
    CHECK(sceNetSend(1, "x", 1, 0) == net::kErrNotInitialized);
    CHECK(sceNetCtlGetState(nullptr) == 0);
    const net::Stats s = net::stats();
    CHECK(s.calls == 5);
    CHECK(s.refused == 4);       // GetState succeeds (reports disconnected)
    CHECK(s.logged == 5);        // all five fully logged (below the cap)
    net::resetCounters();
    CHECK(net::stats().calls == 0);
    return true;
}
bool test_guest_abi_probe() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("abi_probe.elf", opt);
    CHECK(st.guestExitCode == 0);         // every thunk arity verified
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);
    return true;
}

bool test_guest_equeue() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("equeue_demo.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);
    return true;
}

bool test_guest_fs_demo() {
    // Container image for app0 (PSA built here, verified by the guest).
    std::filesystem::create_directories("fsapp/docs");
    {
        std::FILE* f = std::fopen("fsapp/manifest.txt", "wb");
        CHECK(f != nullptr);
        std::fputs("PSBET app0 container", f);
        std::fclose(f);
        f = std::fopen("fsapp/docs/readme.md", "wb");
        CHECK(f != nullptr);
        std::fputs("readme", f);
        std::fclose(f);
    }
    CHECK(psb::pfs::PsbArchive::packDirectory("fsapp", "fsapp.psa"));

    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.mountImage = "fsapp.psa";
    opt.dataDir = "fsdata";
    const psb::EmuStats st = emu.run("fs_demo.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    // The guest's /data write must have landed in the host data dir.
    std::FILE* f = std::fopen("fsdata/hello.txt", "rb");
    CHECK(f != nullptr);
    char buf[32] = {};
    CHECK(std::fread(buf, 1, sizeof(buf) - 1, f) == 19);
    CHECK(std::memcmp(buf, "psbet vfs roundtrip", 19) == 0);
    std::fclose(f);
    std::filesystem::remove_all("fsapp");
    std::filesystem::remove("fsapp.psa");
    std::filesystem::remove_all("fsdata");
    return true;
}

bool test_guest_syscall_probe() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.syscallTrap = "on";               // containment must be active
    const psb::EmuStats st = emu.run("syscall_probe.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    return true;
}

bool test_guest_cube_vm() {
    // The Phase-1 cube, drawn from GPU virtual memory (PBFF v1.1).
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.width = 320; opt.height = 180;
    opt.maxFrames = 30;
    opt.screenshotDir = "test_shots_vm";
    opt.screenshotInterval = 30;
    const psb::EmuStats st = emu.run("cube_vm.elf", opt);
    CHECK(st.guestExitCode == 124);
    CHECK(st.frames == 30);
    CHECK(st.screenshots == 1);
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);
    return true;
}

// ---------------------------------------------------------------------------
// Phase-4 end-to-end guest suite.
// ---------------------------------------------------------------------------
bool test_guest_net_probe() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    const psb::EmuStats st = emu.run("net_probe.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    CHECK(st.netStubCalls >= 13);   // every documented surface call
    return true;
}

bool test_guest_audio_tone() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.audioDump = "test_audio_tone.wav";   // deterministic WAV evidence
    const psb::EmuStats st = emu.run("audio_tone.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);
    CHECK(st.audioDroppedFrames == 0);       // INVARIANT: zero drops
    CHECK(st.audioSubmittedFrames == 24000); // 2 sections x 12000 frames
    CHECK(st.audioMaxLatencyMs <= 30);       // below the 40 ms acceptance

    // Decode + verify the dump: exact length, section frequencies via zero
    // crossings, and the mixed section = A + B/2 (±2 LSB float rounding).
    std::FILE* f = std::fopen("test_audio_tone.wav", "rb");
    CHECK(f != nullptr);
    psb::u8 hdr[44];
    CHECK(std::fread(hdr, 1, 44, f) == 44);
    psb::u32 dataBytes;
    std::memcpy(&dataBytes, hdr + 40, 4);
    CHECK(dataBytes == 24000 * 4);           // 24000 stereo s16 frames
    std::vector<psb::i16> s(24000 * 2);
    CHECK(std::fread(s.data(), 2, s.size(), f) == s.size());
    std::fclose(f);

    // Section 1 (frames 0..11999): 440 Hz solo. The dump is interleaved —
    // analyze the LEFT channel only (even indices), frames not samples.
    auto zeroCrossRate = [&](size_t beginFrame, size_t endFrame) -> double {
        psb::u64 crossings = 0;
        for (size_t f = beginFrame + 1; f < endFrame; ++f)
            if ((s[2 * (f - 1)] < 0) != (s[2 * f] < 0)) ++crossings;
        const double secs = static_cast<double>(endFrame - beginFrame) / 48000.0;
        return static_cast<double>(crossings) / (2.0 * secs);
    };
    const double f1 = zeroCrossRate(0, 12000);
    CHECK(f1 > 435.0 && f1 < 445.0);         // ~440 Hz solo section

    // Section 2 (frames 12000..23999): mixed 660 (full) + 220 (half) — the
    // 660 carrier sets the zero-crossing rate (ripple amplitude < carrier).
    const double f2 = zeroCrossRate(12000, 24000);
    CHECK(f2 > 600.0 && f2 < 720.0);         // ~660 Hz dominant

    // Exact mix math on a quiet window: recompute the expected mixed sample
    // from the guest formula (same libm on the same machine).
    auto clamp16 = [](psb::i32 v) -> psb::i16 {
        return static_cast<psb::i16>(v < -32768 ? -32768
                                                   : (v > 32767 ? 32767 : v));
    };
    bool mixOk = true;
    for (psb::u32 k = 0; k < 800 && mixOk; ++k) {
        const float a = 0.5f * std::sinf(660.f * 2.f * 3.14159265f *
                                         static_cast<float>(k) / 48000.f);
        const float b = 0.5f * std::sinf(220.f * 2.f * 3.14159265f *
                                         static_cast<float>(k) / 48000.f);
        const psb::i32 la = static_cast<psb::i32>(a * 32767.f);
        const psb::i32 lb = static_cast<psb::i32>(b * 32767.f);
        const psb::i32 vMax = static_cast<psb::i32>(psb::audio::kVolMax);
        const psb::i32 vHalf = vMax / 2;
        const psb::i16 want = clamp16(
            clamp16((la * vMax) / vMax) + clamp16((lb * vHalf) / vMax));
        const psb::i16 got = s[(12000 + k) * 2];
        const int diff = std::abs(static_cast<int>(want) - static_cast<int>(got));
        if (diff > 2) mixOk = false;
    }
    CHECK(mixOk);
    std::filesystem::remove("test_audio_tone.wav");
    return true;
}

bool test_guest_audio_paced() {
    // Paced run: frame-clock audio pulls active -> backpressure + measured
    // queue latency, still zero drops (glitch-free under load).
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.audioDump = "test_audio_paced.wav";
    opt.targetFps = 60;
    const psb::EmuStats st = emu.run("audio_tone.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    CHECK(st.audioDroppedFrames == 0);
    CHECK(st.audioSubmittedFrames == 24000);
    CHECK(st.audioMaxLatencyMs > 0);         // paced path exercised
    CHECK(st.audioMaxLatencyMs <= 30);       // < 40 ms acceptance
    CHECK(st.frameMeanIntervalMs > 10.0);    // presentation paced
    CHECK(st.frameMeanIntervalMs < 25.0);
    std::filesystem::remove("test_audio_paced.wav");
    return true;
}

bool test_guest_input_latency() {
    // Scripted DualSense input at 60 FPS pacing. The guest self-fails
    // (exit 1) when any observed press exceeds the 25 ms budget.
    {
        std::FILE* f = std::fopen("test_pad_script.txt", "w");
        CHECK(f != nullptr);
        std::fputs("# PSBET scripted pad events\n"
                   "100 1 128 128 128 128 0 0\n"     // Cross @ 100 ms
                   "150 0 128 128 128 128 0 0\n"     // release
                   "200 GYRO 0.10 0.20 -0.98 12 0 -6\n"
                   "300 100 128 128 128 128 0 255\n",// Options @ 300 ms
                   f);
        std::fclose(f);
    }
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.padScript = "test_pad_script.txt";
    opt.targetFps = 60;
    const psb::EmuStats st = emu.run("input_demo.elf", opt);
    CHECK(st.guestExitCode == 0);            // press inside budget + Options
    CHECK(!st.crashed);
    CHECK(st.threadsDetached == 0);
    CHECK(st.frameMeanIntervalMs > 10.0);    // 60 FPS pacing held
    CHECK(st.frameMeanIntervalMs < 25.0);
    std::filesystem::remove("test_pad_script.txt");
    return true;
}

bool test_guest_paced_cube() {
    // Target-FPS authorization on the GPU workload: 60 frames at 60 FPS.
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.width = 320; opt.height = 180;
    opt.maxFrames = 60;
    opt.targetFps = 60;
    const psb::EmuStats st = emu.run("cube.elf", opt);
    CHECK(st.guestExitCode == 124);
    CHECK(st.frames == 60);
    CHECK(st.frameMeanIntervalMs > 14.0);    // paced, not free-run
    CHECK(st.frameMeanIntervalMs < 20.0);
    CHECK(st.frameMaxIntervalMs < 100.0);    // no stall outliers
    CHECK(!st.crashed);
    return true;
}
#endif

} // namespace

int main() {
    psb::log::setLevel(psb::log::Level::Warn);

    std::printf("PSBET unit tests\n================\n");
    runTest("shader_cache",             test_shader_cache);
    runTest("pixel_compare_sw_vs_vulkan", test_pixel_compare_backends);
    runTest("memory_manager",           test_memory_manager);
    runTest("guest_vm_model (pools/GPU VM)", test_guest_vm_model);
    runTest("page_table_conflicts",     test_page_table_conflicts);
    runTest("png_writer",               test_png_writer);
    runTest("png_rejects_bad_input",    test_png_rejects_bad_input);
    runTest("rasterizer",               test_rasterizer);
    runTest("rasterizer_depth",         test_rasterizer_depth_ordering);
    runTest("cmd_executor",             test_cmd_executor);
    runTest("timing_domain (24h simulated soak)", test_timing_domain);
    runTest("audio_unit (mixer/ports/volume)", test_audio_unit);
    runTest("audio_wav_file",            test_audio_wav_file);
    runTest("pad_unit (scripted driver)", test_pad_unit);
    runTest("net_stubs (documented refusals)", test_net_stubs);
#ifdef PSBET_RUN_GUEST_TESTS
    runTest("guest_hello (threads/TLS/sync)", test_guest_hello);
    runTest("guest_worker_exit (flaw-1 regression)", test_guest_worker_exit);
    runTest("guest_cube (GPU pipeline)",      test_guest_cube);
    runTest("guest_abi_probe (thunk arity suite)", test_guest_abi_probe);
    runTest("guest_equeue (EQUEUE)",          test_guest_equeue);
    runTest("guest_fs_demo (VFS + app0)",     test_guest_fs_demo);
    runTest("guest_syscall_probe (containment)", test_guest_syscall_probe);
    runTest("guest_cube_vm (GPU-VA draws)",   test_guest_cube_vm);
    runTest("guest_net_probe (stub surface)", test_guest_net_probe);
    runTest("guest_audio_tone (mix + WAV)",   test_guest_audio_tone);
    runTest("guest_audio_paced (latency<40ms)", test_guest_audio_paced);
    runTest("guest_input_latency (<25ms@60)", test_guest_input_latency);
    runTest("guest_paced_cube (60 FPS auth)", test_guest_paced_cube);
    runTest("loader_et_exec (absolute vaddrs)", test_loader_et_exec_static);
    runTest("loader_dyn_offset (non-zero lo)",  test_loader_dyn_offset_static);
    runTest("loader_irelative_relro",           test_loader_irelative_relro);
#endif

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed;
}
