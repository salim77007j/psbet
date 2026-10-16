// ============================================================================
//  PSBET — cpu_supervisor.cpp
//  Guest entry/exit control + crash diagnostics.
//
//  Phase-2 changes:
//   - Supervisor-thread scope: requestProcessExit distinguishes the supervisor
//     thread (same-thread longjmp, safe) from guest workers (cooperative
//     per-thread unwind — the Phase-1 cross-thread-longjmp flaw is closed).
//   - Windows guest-hosting shim: SEH vectored exception handler provides the
//     same crash diagnostics as the POSIX signal path, so ELF guests run and
//     fail with full reports on Windows too.
// ============================================================================
#include "cpu_supervisor.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>

#include "core/log.h"
#include "core/kernel_shim.h"
#include "core/elf.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace psb::cpu {

namespace {

jmp_buf     g_returnEnv;
std::string* g_reportSink = nullptr;
u64         g_modBase = 0;
u64         g_modSize = 0;

void buildReport(u64 faultAddr) {
    if (g_reportSink) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "Fatal fault at guest address 0x%llx "
                      "(module base 0x%llx, offset 0x%llx)\n",
                      static_cast<unsigned long long>(faultAddr),
                      static_cast<unsigned long long>(g_modBase),
                      static_cast<unsigned long long>(faultAddr - g_modBase));
        g_reportSink->assign(buf);
    }
    LOG_ERROR("CPU: fatal fault in guest (offset 0x%llx)",
              static_cast<unsigned long long>(faultAddr - g_modBase));
    // Do not unwind through guest frames; exit with a distinctive code.
    std::_Exit(139);
}

#if !defined(_WIN32)
// POSIX fatal-signal diagnostics.
void buildCrashReport(int sig, siginfo_t* info, void* /*ctx*/) {
    buildReport(reinterpret_cast<u64>(info->si_addr));
    (void)sig;
}
#else
// Windows SEH vectored exception handler: first-chance fatal faults inside
// the guest module get the same report + terminate flow as SIGSEGV on Linux.
long CALLBACK guestVectoredHandler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal =
        code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW;
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    const u64 addr = reinterpret_cast<u64>(
        ep->ExceptionRecord->ExceptionInformation[1] != 0
            ? (void*)ep->ExceptionRecord->ExceptionInformation[1]
            : ep->ExceptionRecord->ExceptionAddress);
    buildReport(addr);
    return EXCEPTION_CONTINUE_SEARCH; // not reached (buildReport exits)
}
#endif // _WIN32

} // namespace

void installCrashDiagnostics(std::string* reportSink, u64 moduleBase, u64 moduleSize) {
    g_reportSink = reportSink;
    g_modBase = moduleBase;
    g_modSize = moduleSize;

#if !defined(_WIN32)
    struct sigaction sa{};
    sa.sa_sigaction = buildCrashReport;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
#else
    static void* veh = nullptr;
    if (!veh) {
        veh = AddVectoredExceptionHandler(1, guestVectoredHandler);
        if (!veh)
            LOG_WARN("CPU: AddVectoredExceptionHandler failed — crash "
                     "diagnostics degraded on this Windows host");
    }
#endif
}

GuestRunResult runGuestEntry(const LoadedModule& mod, kernel::KernelState& kernelState) {
    GuestRunResult result;

    installCrashDiagnostics(&result.crashReport, mod.base, mod.imageSize);
    kernel::setProcessJmpBuf(&g_returnEnv);

    // Mark this thread as the supervisor for the exit protocol (flaw fix:
    // worker threads must never longjmp on this thread's env).
    kernel::beginSupervisorScope();

    // Install main-thread TLS for module 1 before any guest code runs.
    kernel::installThreadTls();

    if (setjmp(g_returnEnv) == 0) {
        using GuestEntry = void (*)();
        reinterpret_cast<GuestEntry>(mod.entry)();
        // Guest entry returned without calling ExitProcess: treat as code 0.
        result.exitCode = 0;
        result.exitedViaGuest = true;
    } else {
        result.exitCode = kernelState.exitCode();
        result.exitedViaGuest = true;
        LOG_INFO("CPU: guest exited via sceKernelExitProcess(%d)", result.exitCode);
    }

    kernel::setProcessJmpBuf(nullptr);
    kernel::endSupervisorScope();
    return result;
}

} // namespace psb::cpu
