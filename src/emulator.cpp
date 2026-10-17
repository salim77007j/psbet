// ============================================================================
//  PSBET — emulator.cpp
// ============================================================================
#include "emulator.h"

#include <algorithm>
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
    gpu::setActiveDevice(device.get());

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
            if (gpu::writePng(path, img)) stats.screenshots++;
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
    const u64 framesRendered = device ? device->frameCount() : 0;
    device.reset(); // destroy backend before the memory arena goes away

    // ---- 5. Teardown + stats -----------------------------------------------
    kernel::KernelState::get().shutdownThreads();
    vfs::shutdown();

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

    if (stats.crashed) {
        LOG_ERROR("Emulator: guest crashed — see crash report");
        stats.guestExitCode = 139;
    }
    return stats;
}

} // namespace psb
