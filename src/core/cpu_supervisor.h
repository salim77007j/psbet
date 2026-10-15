// ============================================================================
//  PSBET — cpu_supervisor.h
//  CPU execution supervisor.
//
//  Strategy (Phase 1): native execution. The PS5's CPU is x86-64 (Zen 2) and
//  the host is x86-64, so guest code runs at full native speed and every
//  guest "kernel call" is intercepted at the import/GOT level instead of via
//  instruction translation. The supervisor owns:
//    * guest entry / exit (longjmp-based control flow capture)
//    * fatal-signal crash diagnostics with guest module offsets
//    * main-thread TLS installation
// ============================================================================
#pragma once

#include <string>
#include "psbet/common.h"
#include "core/kernel_shim.h"

namespace psb {

struct LoadedModule;

struct GuestRunResult {
    i32  exitCode = 0;
    bool exitedViaGuest = false;   // true: guest called ExitProcess / returned
    std::string crashReport;
};

namespace cpu {

// Runs the guest entry point on the current thread. Does not return normally
// if the guest exits via sceKernelExitProcess (control returns here instead).
GuestRunResult runGuestEntry(const LoadedModule& mod, kernel::KernelState& kernel);

// Installs host-side fatal-signal diagnostics (SIGSEGV/SIGBUS/SIGILL/SIGFPE).
// Returns a crash report in the provided string when a signal fires.
void installCrashDiagnostics(std::string* reportSink, u64 moduleBase, u64 moduleSize);

} // namespace cpu
} // namespace psb
