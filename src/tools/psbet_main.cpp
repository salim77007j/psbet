// ============================================================================
//  PSBET — psbet_main.cpp
//  CLI front-end: `psbet info <elf>` and `psbet run <elf> [options]`.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "psbet/common.h"
#include "core/log.h"
#include "core/elf.h"
#include "emulator.h"

namespace {

void printBanner() {
    std::printf(
        "PSBET %u.%u.%u — clean-room PS5 runtime for PC (Phase 1)\n"
        "Independent original software. No Sony code, keys or firmware.\n\n",
        psb::kVersionMajor, psb::kVersionMinor, psb::kVersionPatch);
}

void printUsage() {
    std::printf(
        "Usage:\n"
        "  psbet info <image.elf>\n"
        "      Parse and print an ELF image's program headers.\n\n"
        "  psbet run <image.elf> [options]\n"
        "      Load and execute a guest image.\n"
        "Options:\n"
        "  --width N --height N        Framebuffer size (default 1280x720)\n"
        "  --frames N                  Safety frame limit (default 1000000)\n"
        "  --screenshot-dir DIR        Capture PNG screenshots of the framebuffer\n"
        "  --screenshot-interval N     Capture every N frames (default 30)\n"
        "  --verbose                   Debug logging + memory map dump\n");
}

} // namespace

int main(int argc, char** argv) {
    printBanner();
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
            else if (std::strcmp(argv[i], "--screenshot-dir") == 0 && i + 1 < argc)
                opt.screenshotDir = argv[++i];
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
        std::printf("  Guest exit code : %d%s\n", st.guestExitCode,
                    st.guestExitCode == 124 ? " (frame limit)" : "");
        std::printf("  Frames rendered : %llu\n", static_cast<unsigned long long>(st.frames));
        std::printf("  Wall time       : %llu ms\n", static_cast<unsigned long long>(st.elapsedMs));
        std::printf("  Average FPS     : %.1f\n", st.avgFps);
        std::printf("  Screenshots     : %u\n", st.screenshots);
        std::printf("  Peak RSS        : %llu KiB\n", static_cast<unsigned long long>(st.peakRssKiB));
        if (st.crashed) std::printf("  CRASH REPORT    : %s", st.crashReport.c_str());
        std::printf("=========================\n");
        return st.guestExitCode;
    }

    printUsage();
    return 1;
}
