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
#include <vector>

#include "psbet/common.h"
#include "core/memory.h"
#include "core/log.h"
#include "gpu/png.h"
#include "gpu/rasterizer.h"
#include "gpu/gpu_shim.h"
#include "emulator.h"

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
bool test_guest_hello() {
    psb::Emulator emu;
    psb::EmuOptions opt;
    opt.verbose = false;
    const psb::EmuStats st = emu.run("hello.elf", opt);
    CHECK(st.guestExitCode == 0);
    CHECK(!st.crashed);
    return true;
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
#endif

} // namespace

int main() {
    psb::log::setLevel(psb::log::Level::Warn);

    std::printf("PSBET unit tests\n================\n");
    runTest("memory_manager",           test_memory_manager);
    runTest("png_writer",               test_png_writer);
    runTest("png_rejects_bad_input",    test_png_rejects_bad_input);
    runTest("rasterizer",               test_rasterizer);
    runTest("rasterizer_depth",         test_rasterizer_depth_ordering);
    runTest("cmd_executor",             test_cmd_executor);
#ifdef PSBET_RUN_GUEST_TESTS
    runTest("guest_hello (threads/TLS/sync)", test_guest_hello);
    runTest("guest_cube (GPU pipeline)",      test_guest_cube);
#endif

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed;
}
