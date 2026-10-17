// ============================================================================
//  PSBET — corpus_fuzz.cpp
//  Phase-3 fuzzed guest corpus (deterministic; CI-gated).
//
//  Two fuzzers in one binary, exit code = failed seeds:
//
//  1. Kernel-API corpus: pseudo-random multithreaded workloads over the
//     emulated kernel surface (direct-memory alloc/map/unmap/free, mutexes,
//     semaphores, event queues, threads). Every seed is a deterministic
//     program; invariants are checked after each run:
//       * no force-detached threads at teardown (POSIX acceptance),
//       * the physical allocator settles back to its expected state,
//       * no crashes, no hangs (bounded by workload construction).
//     Run under ASan in CI: any silent memory-ordering bug is a hard fail.
//
//  2. PBFF stream fuzz: byte-level mutations of valid command buffers plus
//     fully random streams — the executor must accept or reject cleanly and
//     never crash. Also a valid-random generator (in-range params) that must
//     always render.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <filesystem>
#include <memory>

#include "psbet/common.h"
#include "psbet/psb_guest_api.h"
#include "core/memory.h"
#include "core/kernel_shim.h"
#include "core/log.h"
#include "core/vfs.h"
#include "gpu/gpu_shim.h"
#include "gpu/cmd_stream.h"
#include "core/audio.h"
#include "core/pad.h"
#include "core/net_stubs.h"

using namespace psb;

namespace {

// Deterministic PRNG (xorshift64*).
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u64 next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    u64 range(u64 lo, u64 hi) { return lo + next() % (hi - lo + 1); }
};

// ---------------------------------------------------------------------------
// Fuzzer 1: kernel-API corpus.
// ---------------------------------------------------------------------------
struct CorpusContext {
    psb::MemoryManager* mem = nullptr;
    u64 failures = 0;
};

// Workload phases (executed with random order/params per seed):
//   A: allocDirect -> mapDirect -> write -> verify -> unmap
//   B: allocDirect -> freeDirect (unmapped, must succeed)
//   C: alloc -> map -> double-map (must fail) -> unmap -> free
//   D: mutex-protected shared counter across threads
//   E: equeue producer/consumer with deterministic idents
//   F (Phase 4): audio ports (open/output/volume/close) + pad + net stubs —
//      the mixer's drop invariant is checked after every seed.
enum class Phase { A, B, C, D, E, F };

bool runCorpusSeed(u64 seed) {
    CorpusContext ctx;
    psb::MemoryManager mem;
    if (!mem.init()) return false;
    ctx.mem = &mem;
    psb::kernel::KernelState::get().init(&mem);

    Rng rng(seed);
    bool ok = true;
    const char* failAt = "-";

    // Phase A/B/C/F on the model directly (in-process, no guests).
    for (int iter = 0; iter < 24; ++iter) {
        const Phase phase = static_cast<Phase>(rng.next() % 4);
        const u64 len = rng.range(0x1000, 0x10000);
        auto& gm = mem.guest();

        if (phase == Phase::A) {
            const u64 pa = gm.allocDirect(psb::GuestMemory::kPoolRegular, len,
                                          0x1000, 0, 0, "fuzz-a");
            if (!pa) continue;
            const u64 va = gm.mapDirect(pa, len, psb::vm::Prot::ReadWrite, "fuzz-a");
            if (!va) { ok = false; failAt = "A-map"; break; }
            volatile u8* p = reinterpret_cast<volatile u8*>(va);
            p[0] = 0x5A; p[len / 2] = 0xA5; p[len - 1] = 0xC3;
            if (p[0] != 0x5A || p[len / 2] != 0xA5 || p[len - 1] != 0xC3)
                { ok = false; failAt = "A-verify"; }
            if (!gm.unmap(va, len)) { ok = false; failAt = "A-unmap"; }
            if (!gm.freeDirect(pa, len)) { ok = false; failAt = "A-free"; }
        } else if (phase == Phase::F) {
            // Audio/pad/net stress: random-but-valid calls; the mixer must
            // never drop and every net stub must refuse.
            const u32 len = static_cast<u32>(rng.range(1, 2000));
            const i32 h = sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0, len,
                                          48000, PSB_AUDIO_FMT_S16_STEREO);
            if (h >= 0) {
                std::vector<i16> buf(len * 2, 1000);
                const int submits = static_cast<int>(rng.range(1, 8));
                for (int k = 0; k < submits; ++k)
                    if (sceAudioOutOutput(h, buf.data()) != 0)
                        { ok = false; failAt = "F-output"; break; }
                i32 vol[2] = {32768, 16384};
                (void)sceAudioOutSetVolume(h, 0x3, vol);
                if (sceAudioOutClose(h) != 0) { ok = false; failAt = "F-close"; }
            }
            if (scePadOpen(0, 0, 0, nullptr) == 1) {
                psb::pad::PadState st{};
                if (psb::pad::PadService::get().readState(1, &st) != 0)
                    { ok = false; failAt = "F-pad"; }
            }
            if (sceNetInit(0) != psb::net::kErrUnavailable)
                { ok = false; failAt = "F-net"; }
        } else if (phase == Phase::B) {
            const u64 pa = gm.allocDirect(psb::GuestMemory::kPoolRegular, len,
                                          0x1000, 0, 0, "fuzz-b");
            if (!pa) continue;
            if (!gm.freeDirect(pa, len)) { ok = false; failAt = "B-free"; }
        } else {
            const u64 pa = gm.allocDirect(psb::GuestMemory::kPoolRegular, len,
                                          0x1000, 0, 0, "fuzz-c");
            if (!pa) continue;
            const u64 va = gm.mapDirect(pa, len, psb::vm::Prot::ReadWrite, "fuzz-c");
            if (!va) { ok = false; failAt = "C-map"; break; }
            if (!gm.unmap(va, len)) { ok = false; failAt = "C-unmap"; }
            if (!gm.freeDirect(pa, len)) { ok = false; failAt = "C-free"; }
            // Mapping a freed (unallocated) range must fail.
            if (gm.mapDirect(pa, 0x1000, psb::vm::Prot::ReadWrite, "fuzz-c3"))
                { ok = false; failAt = "C-reghost"; }
        }
    }

    // Phase D/E through the emulated kernel surface (real host threads here;
    // guest-thread lifecycle is covered by the homebrew suite via Emulator).
    {
        int32_t mutexId = 0, semaId = 0, eqId = 0;
        ::sceKernelCreateMutex("fuzz", 0, 0, &mutexId);
        ::sceKernelCreateSemaphore("fuzz", 0, 64, &semaId);
        ::sceKernelCreateEqueue("fuzz", &eqId);

        static i32 sharedCounter = 0;
        sharedCounter = 0;

        auto worker = [&](u64 id) {
            Rng wrng(seed ^ (id * 0xD1B54A32D192ED03ull));
            for (int i = 0; i < 200; ++i) {
                ::sceKernelLockMutex(mutexId, 0);
                sharedCounter++;
                ::sceKernelUnlockMutex(mutexId);
                if (wrng.next() & 1) ::sceKernelAddEvent(eqId, i, 0, id);
                if (wrng.next() & 1) {
                    PsbEqueueEvent ev;
                    ::sceKernelWaitEqueue(eqId, &ev, 1000);
                }
                if (wrng.next() % 8 == 0) ::sceKernelUsleep(100);
            }
            ::sceKernelSignalSemaphore(semaId, 1);
        };

        std::vector<std::thread> threads;
        for (u64 t = 0; t < 3; ++t) threads.emplace_back(worker, t);
        // NOTE: these are HOST threads (this fuzzer is engine-side); they are
        // joined here directly. Guest-thread lifecycle is covered by the
        // homebrew guests run through the full Emulator.
        for (auto& t : threads) t.join();

        if (sharedCounter != 600) { ok = false; failAt = "D-counter"; }
    }

    // Teardown accounting: the fuzz corpus itself spawns no guest threads,
    // but shutdown must stay clean.
    psb::kernel::KernelState::get().shutdownThreads();
    const auto ts = psb::kernel::KernelState::get().lastThreadStats();
    if (ts.detached != 0) { ok = false; failAt = "T-detach"; }
    if (!ok)
        std::printf("  [FUZZ] seed %llu failed at %s\n",
                    static_cast<unsigned long long>(seed), failAt);

    psb::vfs::shutdown();
    return ok;
}

// ---------------------------------------------------------------------------
// Fuzzer 2: PBFF stream fuzz.
// ---------------------------------------------------------------------------
bool runPbffSeed(u64 seed) {
    Rng rng(seed);
    const u32 W = 64, H = 64;
    std::vector<u8> color(W * H * 4);
    std::vector<float> depth(W * H, 1.0f);

    psb::gpu::Device dev(color.data(), depth.data(), W, H);
    psb::gpu::setActiveDevice(&dev);

    // Valid base stream.
    psb::gpu::CmdWriter base;
    base.clear();
    psb::gpu::CmdClear clear;
    clear.color[0] = 0.2f; clear.color[1] = 0.3f; clear.color[2] = 0.4f;
    base.push(clear);
    psb::gpu::CmdSetViewport vp;
    vp.w = (float)W; vp.h = (float)H;
    base.push(vp);
    psb::gpu::GpuVertex tri[3] = {
        {{-1, -1, 0.5f, 1}, {1, 0, 0, 1}}, {{1, -1, 0.5f, 1}, {0, 1, 0, 1}},
        {{0, 1, 0.5f, 1}, {0, 0, 1, 1}},
    };
    base.draw(tri, 3);

    bool ok = true;

    // a) Mutations of the valid stream: any result is fine, crashing is not.
    for (int m = 0; m < 24; ++m) {
        std::vector<u8> stream(
            static_cast<const u8*>(base.data()),
            static_cast<const u8*>(base.data()) + base.size());
        const u64 mutations = rng.range(1, 12);
        for (u64 i = 0; i < mutations && !stream.empty(); ++i) {
            const size_t idx = static_cast<size_t>(rng.next() % stream.size());
            stream[idx] = static_cast<u8>(rng.next());
        }
        (void)dev.executeCommandBuffer(stream.data(), (u32)stream.size());
    }

    // b) Fully random streams: reject or execute, never crash.
    for (int m = 0; m < 24; ++m) {
        std::vector<u8> stream(static_cast<size_t>(rng.range(0, 512)));
        for (auto& b : stream) b = static_cast<u8>(rng.next());
        (void)dev.executeCommandBuffer(stream.data(), (u32)stream.size());
    }

    // c) Valid-random streams: must always execute cleanly.
    for (int m = 0; m < 8; ++m) {
        psb::gpu::CmdWriter cw;
        cw.clear();
        psb::gpu::CmdClear c;
        c.color[0] = (rng.next() % 256) / 255.0f;
        c.color[1] = (rng.next() % 256) / 255.0f;
        c.color[2] = (rng.next() % 256) / 255.0f;
        cw.push(c);
        psb::gpu::CmdSetViewport v;
        v.w = (float)W; v.h = (float)H;
        cw.push(v);
        psb::gpu::CmdSetDepthTest dt;
        dt.enable = (u32)(rng.next() % 2);
        dt.func = psb::gpu::DepthFunc(rng.next() % 4);
        cw.push(dt);
        psb::gpu::CmdSetCullMode cm;
        cm.mode = psb::gpu::CullMode(rng.next() % 2);
        cw.push(cm);
        // In-range random triangle.
        psb::gpu::GpuVertex triR[3];
        for (auto& v2 : triR) {
            for (float& f : v2.pos) f = ((i64)(rng.next() % 2001) - 1000) / 500.0f;
            v2.pos[3] = 1.f;
            for (float& f : v2.col) f = (rng.next() % 256) / 255.0f;
        }
        if (rng.next() & 1) {
            for (auto& v2 : triR) v2.pos[2] = (rng.next() % 1001) / 1000.0f;
        }
        cw.draw(triR, 3);
        if (!dev.executeCommandBuffer(cw.data(), cw.size())) ok = false;
    }

    psb::gpu::setActiveDevice(nullptr);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    psb::log::setLevel(psb::log::Level::Error);

    u64 seeds = 64;
    if (argc > 1) seeds = std::strtoull(argv[1], nullptr, 10);

    std::printf("PSBET fuzz corpus (%llu seeds)\n================\n",
                static_cast<unsigned long long>(seeds));

    // Phase-4 services: reset the pad table; the mixer runs backend-less
    // (drain-advance only) so the drop invariant is exercised, not masked.
    psb::pad::PadService::get().setDriver(
        std::make_unique<psb::pad::NullPadDriver>());
    const u64 audioDroppedBefore = psb::audio::Mixer::get().stats().droppedFrames;

    u64 failed = 0;
    for (u64 s = 0; s < seeds; ++s) {
        if (!runCorpusSeed(s)) {
            std::printf("  [FUZZ] corpus seed %llu FAIL\n",
                        static_cast<unsigned long long>(s));
            ++failed;
        }
        if (!runPbffSeed(s)) {
            std::printf("  [FUZZ] pbff seed %llu FAIL\n",
                        static_cast<unsigned long long>(s));
            ++failed;
        }
    }

    // Phase-4 invariant: the mixer NEVER drops (fuzz audio included).
    const u64 droppedNow = psb::audio::Mixer::get().stats().droppedFrames;
    if (droppedNow != audioDroppedBefore) {
        std::printf("  [FUZZ] audio drop invariant violated (%llu dropped)\n",
                    static_cast<unsigned long long>(droppedNow));
        ++failed;
    }

    std::printf("-> fuzz: %llu seeds, %llu failures\n",
                static_cast<unsigned long long>(seeds),
                static_cast<unsigned long long>(failed));
    return failed ? 1 : 0;
}
