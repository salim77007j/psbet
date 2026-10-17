// ============================================================================
//  PSBET — psbet_main.cpp
//  CLI front-end:
//    psbet info <elf>            — parse and print an ELF image
//    psbet gpu                   — probe the host Vulkan stack
//    psbet mkfsa <dir> <out>     — pack a directory into a PSBET Archive
//    psbet run <elf> [options]   — load and execute a guest image
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "psbet/common.h"
#include "core/log.h"
#include "core/elf.h"
#include "core/pfs.h"
#include "emulator.h"
#include "gpu/vulkan_device.h"

namespace {

void printBanner() {
    std::printf(
        "PSBET %u.%u.%u — clean-room PS5 runtime for PC (Phase 4)\n"
        "Independent original software. No Sony code, keys or firmware.\n\n",
        psb::kVersionMajor, psb::kVersionMinor, psb::kVersionPatch);
}

void printUsage() {
    std::printf(
        "Usage:\n"
        "  psbet info <image.elf>\n"
        "      Parse and print an ELF image's program headers.\n\n"
        "  psbet gpu\n"
        "      Probe the host Vulkan stack (devices, driver versions).\n\n"
        "  psbet mkfsa <dir> <out.psa>\n"
        "      Pack a directory into a read-only PSBET Archive image that\n"
        "      `run --mount-pfs` mounts at app0:/ (see docs/GUEST_ABI.md §8).\n\n"
        "  psbet run <image.elf> [options]\n"
        "      Load and execute a guest image.\n"
        "Options:\n"
        "  --width N --height N        Framebuffer size (default 1280x720)\n"
        "  --frames N                  Safety frame limit (default 1000000)\n"
        "  --screenshot-dir DIR        Capture PNG screenshots of the framebuffer\n"
        "  --screenshot-interval N     Capture every N frames (default 30)\n"
        "  --backend NAME              auto | software | vulkan (default auto)\n"
        "  --mount-pfs IMAGE           Mount a read-only container at app0:/\n"
        "  --data-dir DIR              Mount a writable host dir at /data\n"
        "  --syscall-trap MODE         on | off | required (default on)\n"
        "  --fps N                     Authorize a target frame rate (60/120);\n"
        "                              0 = free run (default). Enables paced\n"
        "                              audio consumption (audio-video sync).\n"
        "  --vsync on|off              Present-queue backpressure (with --fps)\n"
        "  --audio-dump PATH           Dump the mixed master stream as WAV\n"
        "  --pad-script PATH           Scripted pad events (deterministic input)\n"
        "  --window on|off             SDL3 presentation window (if built in)\n"
        "  --verbose                   Debug logging + memory map dump\n");
}

} // namespace

int main(int argc, char** argv) {
    printBanner();

    if (argc >= 2 && std::strcmp(argv[1], "gpu") == 0) {
        std::fputs(psb::gpu::VulkanDevice::probeReport().c_str(), stdout);
        std::fputc('\n', stdout);
        return psb::gpu::VulkanDevice::available() ? 0 : 1;
    }

    if (argc >= 4 && std::strcmp(argv[1], "mkfsa") == 0) {
        const psb::Result r = psb::pfs::PsbArchive::packDirectory(argv[2], argv[3])
            ? psb::Result::ok()
            : psb::Result::fail(psb::Status::IoError, "packing failed (see log)");
        if (!r) {
            std::fprintf(stderr, "error: %s\n", r.message.c_str());
            return 2;
        }
        std::printf("packed %s -> %s\n", argv[2], argv[3]);
        return 0;
    }

    if (argc < 3) {
        printUsage();
        return 1;
    }

    const std::string cmd = argv[1];
    const std::string path = argv[2];

    if (cmd == "info") {
        std::string report;
        psb::Result r = psb::ElfLoader::inspect(path, report);
        std::fputs(report.c_str(), stdout);
        if (!r) {
            std::fprintf(stderr, "error: %s\n", r.message.c_str());
            return 2;
        }
        return 0;
    }

    if (cmd == "run") {
        psb::EmuOptions opt;
        for (int i = 3; i < argc; ++i) {
            const auto need = [&](const char* name) -> const char* {
                if (std::strcmp(argv[i], name) != 0 || i + 1 >= argc) return nullptr;
                return argv[++i];
            };
            if (const char* v = need("--width"))  opt.width = static_cast<psb::u32>(std::atoi(v));
            else if (const char* v = need("--height")) opt.height = static_cast<psb::u32>(std::atoi(v));
            else if (const char* v = need("--frames")) opt.maxFrames = static_cast<psb::u32>(std::atoi(v));
            else if (const char* v = need("--screenshot-interval"))
                opt.screenshotInterval = static_cast<psb::u32>(std::atoi(v));
            else if (const char* v = need("--data-dir"))
                opt.dataDir = v;
            else if (const char* v = need("--syscall-trap"))
                opt.syscallTrap = v;
            else if (const char* v = need("--fps"))
                opt.targetFps = static_cast<psb::u64>(std::atoll(v));
            else if (const char* v = need("--audio-dump"))
                opt.audioDump = v;
            else if (const char* v = need("--pad-script"))
                opt.padScript = v;
            else if (std::strcmp(argv[i], "--vsync") == 0 && i + 1 < argc)
                opt.vsync = (std::strcmp(argv[++i], "on") == 0);
            else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc)
                opt.window = argv[++i];
            else if (std::strcmp(argv[i], "--screenshot-dir") == 0 && i + 1 < argc)
                opt.screenshotDir = argv[++i];
            else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
                opt.backend = argv[++i];
            else if (std::strcmp(argv[i], "--mount-pfs") == 0 && i + 1 < argc)
                opt.mountImage = argv[++i];
            else if (std::strcmp(argv[i], "--verbose") == 0)
                opt.verbose = true;
            else {
                std::fprintf(stderr, "unknown option: %s\n", argv[i]);
                printUsage();
                return 1;
            }
        }

        psb::log::setVerbose(opt.verbose);
        psb::Emulator emu;
        const psb::EmuStats st = emu.run(path, opt);

        std::printf("\n=== PSBET run summary ===\n");
        std::printf("  GPU backend     : %s\n", st.backendUsed.c_str());
        std::printf("  Presenter       : %s\n", st.presenterUsed.c_str());
        std::printf("  Guest exit code : %d%s\n", st.guestExitCode,
                    st.guestExitCode == 124 ? " (frame limit)" : "");
        std::printf("  Frames rendered : %llu\n", static_cast<unsigned long long>(st.frames));
        std::printf("  Wall time       : %llu ms\n", static_cast<unsigned long long>(st.elapsedMs));
        std::printf("  Average FPS     : %.1f\n", st.avgFps);
        std::printf("  Frame interval  : mean %.2f ms, max %.2f ms, vsync waits %u\n",
                    st.frameMeanIntervalMs, st.frameMaxIntervalMs, st.vsyncWaits);
        std::printf("  Audio backend   : %s (submitted %llu, mixed %llu, dropped %llu,\n"
                    "                    max latency %u ms, backpressure waits %u)\n",
                    st.audioBackendUsed.c_str(),
                    static_cast<unsigned long long>(st.audioSubmittedFrames),
                    static_cast<unsigned long long>(st.audioMixedFrames),
                    static_cast<unsigned long long>(st.audioDroppedFrames),
                    st.audioMaxLatencyMs, st.audioBackpressureWaits);
        std::printf("  Net stubs       : %llu calls refused (no network)\n",
                    static_cast<unsigned long long>(st.netStubCalls));
        std::printf("  Screenshots     : %u\n", st.screenshots);
        std::printf("  Peak RSS        : %llu KiB\n", static_cast<unsigned long long>(st.peakRssKiB));
        std::printf("  Guest threads   : %u joined, %u force-detached, %u parked\n",
                    st.threadsJoined, st.threadsDetached, st.threadsParked);
        std::printf("  Physical memory : %llu KiB live at exit\n",
                    static_cast<unsigned long long>(st.memoryUsedBytes / 1024));
        if (st.crashed) std::printf("  CRASH REPORT    : %s", st.crashReport.c_str());
        std::printf("=========================\n");
        return st.guestExitCode;
    }

    printUsage();
    return 1;
}
