// ============================================================================
//  PSBET — emulator.cpp
// ============================================================================
#include "emulator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "core/log.h"
#include "core/memory.h"
#include "core/elf.h"
#include "core/kernel_shim.h"
#include "core/cpu_supervisor.h"
#include "core/vfs.h"
#include "core/syscall_trap.h"
#include "core/timing.h"
#include "core/audio.h"
#include "core/pad.h"
#include "core/net_stubs.h"
#include "core/pacer.h"
#include "gpu/gpu_shim.h"
#include "gpu/png.h"
#include "gpu/vulkan_device.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace psb {

namespace {

u64 nowUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Peak RSS (VmHWM) from /proc/self/status — Linux only; 0 elsewhere.
u64 peakRssKiB() {
#if !defined(_WIN32)
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    u64 kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long v = 0;
        if (std::sscanf(line, "VmHWM: %llu kB", &v) == 1) { kb = v; break; }
    }
    std::fclose(f);
    return kb;
#else
    return 0;
#endif
}

void* resolveImports(const char* name) {
    // Order matters: engine shims first, then host libc fallback.
    if (void* p = kernel::KernelState::get().find(name)) return p;
    if (void* p = gpu::findGpuExport(name)) return p;
#if !defined(_WIN32)
    if (void* p = dlsym(RTLD_DEFAULT, name)) return p;
#endif
    return nullptr;
}

} // namespace

EmuStats Emulator::run(const std::string& elfPath, const EmuOptions& opt) {
    EmuStats stats;
    const u64 t0 = nowUs();

    // ---- 0. Filesystem virtualization + syscall containment -----------------
    vfs::shutdown();   // reset any state from a previous run in-process
    if (!opt.dataDir.empty()) vfs::initDataDir(opt.dataDir);
    if (!opt.mountImage.empty() && !vfs::mountImage(opt.mountImage)) {
        LOG_ERROR("Emulator: app0 mount failed: %s", opt.mountImage.c_str());
        stats.guestExitCode = 2;
        return stats;
    }
    net::resetCounters();

    syscall_trap::Mode trapMode = syscall_trap::Mode::On;
    if (opt.syscallTrap == "off") trapMode = syscall_trap::Mode::Off;
    else if (opt.syscallTrap == "required") trapMode = syscall_trap::Mode::Required;
    kernel::KernelState::get().setSyscallTrapMode(trapMode);

    // ---- 1. Guest address space ------------------------------------------
    MemoryManager mem;
    if (!mem.init()) {
        LOG_ERROR("Emulator: memory subsystem init failed");
        return stats;
    }
    kernel::KernelState::get().init(&mem);
    kernel::KernelState::get().setProcessStart(t0);

    // ---- 1b. Audio / input / pacing services (Phase 4) -----------------------
    // Audio backend: WAV dump when requested (deterministic CI evidence),
    // SDL3 playback when built, null otherwise.
    std::unique_ptr<audio::IAudioBackend> audioBackend;
    if (!opt.audioDump.empty())
        audioBackend = std::make_unique<audio::WavDumpBackend>(opt.audioDump);
#ifdef PSBET_WITH_SDL3
    else
        audioBackend = std::make_unique<audio::SdlBackend>();
#endif
    if (!audioBackend)
        audioBackend = std::make_unique<audio::NullBackend>();
    audio::Mixer::get().setBackend(std::move(audioBackend));

    // Input driver: scripted events (deterministic) else SDL gamepads (when
    // built) else null (all pads disconnected).
    std::unique_ptr<pad::IPadDriver> padDriver;
    if (!opt.padScript.empty()) {
        auto scripted = std::make_unique<pad::ScriptedPadDriver>();
        if (!scripted->loadFromFile(opt.padScript.c_str())) {
            LOG_ERROR("Emulator: pad script invalid: %s", opt.padScript.c_str());
            audio::Mixer::get().clearBackend();
            stats.guestExitCode = 2;
            return stats;
        }
        LOG_INFO("Emulator: scripted pad driver active (%u events)",
                 scripted->eventCount());
        padDriver = std::move(scripted);
#ifdef PSBET_WITH_SDL3
    } else {
        padDriver = std::make_unique<pad::SdlGamepadDriver>();
#endif
    }
    if (!padDriver)
        padDriver = std::make_unique<pad::NullPadDriver>();
    pad::PadService::get().setDriver(std::move(padDriver));

    // Presentation: SDL3 window when built + requested, offscreen otherwise.
    av::OffscreenPresenter offscreenPresenter;
    av::IPresenter* presenter = &offscreenPresenter;
    stats.presenterUsed = offscreenPresenter.name();
#ifdef PSBET_WITH_SDL3
    std::unique_ptr<av::SdlPresenter> windowPresenter;
    if (opt.window == "on") {
        windowPresenter = std::make_unique<av::SdlPresenter>(opt.width, opt.height);
        if (windowPresenter->valid()) {
            presenter = windowPresenter.get();
            stats.presenterUsed = presenter->name();
        } else {
            LOG_WARN("Emulator: SDL window unavailable — staying offscreen");
            windowPresenter.reset();
        }
    }
#else
    if (opt.window == "on")
        LOG_WARN("Emulator: --window requested but this build has no SDL3 "
                 "(PSBET_WITH_SDL3) — staying offscreen");
#endif

    // Frame pacing: target-FPS authorization (console boost-mode analogue).
    av::FramePacer pacer;
    pacer.configure(opt.targetFps, opt.vsync);

    // Phase-5 raster tuning: band worker count (0 = auto). Must be set
    // before the first draw so the pool forms with the requested size.
    if (opt.gpuThreads != 0) gpu::Rasterizer::setBandThreads(opt.gpuThreads);

    // ---- 2. Load guest image ----------------------------------------------
    std::vector<ImportFn> resolvers{&resolveImports};
    ElfLoader loader(mem, resolvers);
    LoadedModule mod;

    u32 moduleId = 1;
    Result r = loader.load(elfPath, moduleId, mod);
    if (!r) {
        LOG_ERROR("Emulator: load failed: %s", r.message.c_str());
        stats.guestExitCode = 2;
        return stats;
    }
    kernel::KernelState::get().registerTlsModule(moduleId,
        mod.tlsImage.empty() ? nullptr : mod.tlsImage.data(), mod.tlsMemSize);

    LOG_INFO("Emulator: image mapped at 0x%llx (size %llu KiB, entry 0x%llx)",
             static_cast<unsigned long long>(mod.base),
             static_cast<unsigned long long>(mod.imageSize / 1024),
             static_cast<unsigned long long>(mod.entry));
    if (opt.verbose) LOG_INFO("Memory map:\n%s", mem.regionMap().c_str());

    // ---- 3. GPU device ----------------------------------------------------
    const size_t colorBytes = static_cast<size_t>(opt.width) * opt.height * 4;
    const size_t depthBytes = static_cast<size_t>(opt.width) * opt.height * sizeof(float);
    const u64 fbAddr = mem.allocate(colorBytes + depthBytes, 256, "framebuffer",
                                    vm::Prot::ReadWrite);
    if (!fbAddr) {
        LOG_ERROR("Emulator: framebuffer allocation failed");
        stats.guestExitCode = 3;
        return stats;
    }
    auto* colorBuf = reinterpret_cast<u8*>(fbAddr);
    auto* depthBuf = reinterpret_cast<float*>(fbAddr + colorBytes);

    // ---- backend selection (auto | software | vulkan) ----------------------
    std::string wantBackend = opt.backend.empty() ? "auto" : opt.backend;
    std::for_each(wantBackend.begin(), wantBackend.end(),
                  [](char& c) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    std::unique_ptr<gpu::IRasterBackend> backend;
    if (wantBackend == "vulkan" || wantBackend == "auto") {
        if (!gpu::VulkanDevice::selfTest()) {
            LOG_WARN("Emulator: Vulkan self-test failed — the host Vulkan stack "
                     "cannot render reliably in this environment");
            if (wantBackend == "vulkan") {
                LOG_ERROR("Emulator: Vulkan backend requested but failed self-test");
                stats.guestExitCode = 3;
                return stats;
            }
        } else {
            backend = gpu::VulkanDevice::create(colorBuf, opt.width, opt.height);
            if (!backend && wantBackend == "vulkan") {
                LOG_ERROR("Emulator: Vulkan backend requested but unavailable");
                stats.guestExitCode = 3;
                return stats;
            }
        }
    }

    std::unique_ptr<gpu::Device> device;
    if (backend) {
        device = std::make_unique<gpu::Device>(colorBuf, opt.width, opt.height,
                                               std::move(backend));
        stats.backendUsed = "vulkan";
        LOG_INFO("Emulator: GPU backend: Vulkan");
    } else {
        device = std::make_unique<gpu::Device>(colorBuf, depthBuf, opt.width, opt.height);
        stats.backendUsed = "software";
        LOG_INFO("Emulator: GPU backend: software rasterizer%s",
                 wantBackend == "auto" ? " (auto — Vulkan unavailable)" : "");
    }
    device->setMaxFrames(opt.maxFrames);
    device->setAddressSpace(&mem.guest());   // GPU-VA draws (PBFF v1.1)
    device->setFrameClock(&pacer, &audio::Mixer::get());
    device->setPresenter(presenter);
    // Phase-5 submit path: pipelined by default (--gpu-sync restores the
    // inline path). Direct Device users (unit tests) stay synchronous.
    device->setPipelined(!opt.gpuSync);
    // Hardware backends skip the per-frame readback when nothing consumes
    // the host color buffer (no screenshots, offscreen presentation).
    device->setReadbackHint(!opt.screenshotDir.empty() ||
                            presenter != static_cast<av::IPresenter*>(&offscreenPresenter));
    gpu::setActiveDevice(device.get());

    std::atomic<u32> screenshotsTaken{0};   // hook runs on the GPU worker now
    if (!opt.screenshotDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt.screenshotDir, ec);
        device->setFrameHook([&](u64 frame) {
            if (opt.screenshotInterval == 0) return;
            if (frame % opt.screenshotInterval != 0) return;
            gpu::Image img;
            img.width = opt.width;
            img.height = opt.height;
            img.rgba.assign(colorBuf, colorBuf + colorBytes);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/frame_%04llu.png",
                          opt.screenshotDir.c_str(),
                          static_cast<unsigned long long>(frame));
            if (gpu::writePng(path, img)) ++screenshotsTaken;
            else LOG_WARN("Emulator: screenshot write failed: %s", path);
        });
    }

    // ---- 4. Run guest initializers, then enter guest code ------------------
    r = loader.runInitializers(mod);
    if (!r) {
        LOG_ERROR("Emulator: guest initializers failed: %s", r.message.c_str());
        stats.guestExitCode = 2;
        gpu::setActiveDevice(nullptr);
        return stats;
    }

    LOG_INFO("Emulator: entering guest (%s)", elfPath.c_str());
    GuestRunResult run = cpu::runGuestEntry(mod, kernel::KernelState::get());
    gpu::setActiveDevice(nullptr);
    // Phase-5: drain + join the GPU queue workers BEFORE reading stats —
    // the last submitted frames may still be in flight when the guest exits.
    device->shutdownQueues();
    const u64 framesRendered = device ? device->frameCount() : 0;
    const gpu::Device::QueueStats qs = device->queueStats();
    device.reset(); // destroy backend before the memory arena goes away

    // ---- 5. Teardown + stats -----------------------------------------------
    kernel::KernelState::get().shutdownThreads();
    vfs::shutdown();
    // Snapshot service stats BEFORE teardown (clearBackend resets counters).
    const char* audioBackendName = audio::Mixer::get().backendName();
    const audio::Mixer::Stats audioStats = audio::Mixer::get().stats();
    const av::FramePacer::Stats pacerStats = pacer.stats();
    const net::Stats netStats = net::stats();
    pad::PadService::get().clearDriver();
    audio::Mixer::get().clearBackend();   // flush WAV dumps, stop consumers

    stats.guestExitCode = run.exitCode;
    stats.exitedViaGuest = run.exitedViaGuest;
    stats.crashed = !run.crashReport.empty();
    stats.crashReport = run.crashReport;
    stats.frames = framesRendered;
    stats.elapsedMs = (nowUs() - t0) / 1000;
    stats.avgFps = stats.elapsedMs > 0
        ? static_cast<double>(stats.frames) * 1000.0 / static_cast<double>(stats.elapsedMs)
        : 0.0;
    stats.peakRssKiB = peakRssKiB();

    const kernel::ThreadStats ts = kernel::KernelState::get().lastThreadStats();
    stats.threadsJoined = ts.joined;
    stats.threadsDetached = ts.detached;
    stats.threadsParked = ts.parked;
    stats.memoryUsedBytes = mem.bytesAllocated();

    // Phase-4 service accounting (snapshotted before teardown above).
    stats.audioBackendUsed = audioBackendName;
    stats.audioSubmittedFrames = audioStats.submittedFrames;
    stats.audioMixedFrames = audioStats.mixedFrames;
    stats.audioDroppedFrames = audioStats.droppedFrames;
    stats.audioMaxLatencyMs = audioStats.maxLatencyMs;
    stats.audioBackpressureWaits = audioStats.backpressureWaits;
    stats.frameMeanIntervalMs = pacerStats.meanIntervalMs;
    stats.frameMaxIntervalMs = pacerStats.maxIntervalMs;
    stats.vsyncWaits = pacerStats.vsyncWaits;
    stats.netStubCalls = netStats.calls;

    // Phase-5 GPU queue / raster accounting (queues drained before this).
    stats.screenshots = screenshotsTaken.load();
    stats.gpuPipelined = qs.pipelined;
    stats.gpuEnqueued = qs.gfx.enqueued;
    stats.gpuCompleted = qs.gfx.completed;
    stats.gpuBackpressureWaits = qs.gfx.backpressureWaits;
    stats.gpuMaxInFlight = qs.gfx.maxInFlight;
    stats.gpuMaxInFlightCap = gpu::GpuQueue::kDefaultMaxInFlight;
    stats.gpuExecuteUs = qs.gfx.executeUs;
    stats.gpuComputeSubmits = qs.compute.enqueued;
    stats.rasterThreads = gpu::Rasterizer::bandThreads();
    stats.rasterSimd = gpu::Rasterizer::simdEnabled() &&
                       gpu::Rasterizer::simdCompiled() &&
                       gpu::Rasterizer::hostHasAvx2();

    if (stats.crashed) {
        LOG_ERROR("Emulator: guest crashed — see crash report");
        stats.guestExitCode = 139;
    }
    return stats;
}

} // namespace psb
