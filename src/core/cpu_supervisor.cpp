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

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <thread>

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

[[maybe_unused]] jmp_buf g_returnEnv; // legacy: kept for reference (Ctx owns the live env now)
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
    const u64 faultPc = reinterpret_cast<u64>(ep->ExceptionRecord->ExceptionAddress);
    const char* name = "unknown";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    name = "ACCESS_VIOLATION"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: name = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_PRIV_INSTRUCTION:    name = "PRIV_INSTRUCTION"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  name = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_STACK_OVERFLOW:      name = "STACK_OVERFLOW"; break;
        case 0xC0000409:                    name = "STACK_BUFFER_OVERRUN"; break;
        case 0xC0000602:                    name = "FAIL_FAST_EXCEPTION"; break;
        default: break;
    }
    LOG_ERROR("CPU: Windows exception %s (0x%08lx) at host 0x%llx (module offset 0x%llx)",
              name, static_cast<unsigned long>(code),
              static_cast<unsigned long long>(faultPc),
              static_cast<unsigned long long>(faultPc - g_modBase));
    const bool fatal =
        code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == 0xC0000409 || code == 0xC0000602;
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    u64 addr = faultPc;
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[1] != 0)
        addr = ep->ExceptionRecord->ExceptionInformation[1];
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

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Windows hosting model: NO longjmp. MSVC's SEH dispatch raises
// STATUS_INVALID_DISPATCH when unwinding interacts with GCC-compiled guest
// frames (no MSVC unwind metadata). Instead, the guest runs on a dedicated
// thread; guest-initiated exits record the code and signal the supervisor,
// which returns normally. The guest thread parks in host code (never
// unwinding guest frames) and is detached at shutdown — the process is
// exiting anyway. Crash diagnostics come from the VEH handler above.
// ---------------------------------------------------------------------------
GuestRunResult runGuestEntry(const LoadedModule& mod, kernel::KernelState& kernelState) {
    GuestRunResult result;
    installCrashDiagnostics(&result.crashReport, mod.base, mod.imageSize);
    kernel::beginSupervisorScope();
    kernel::installThreadTls();

    std::mutex m;
    std::condition_variable cv;
    bool guestDone = false;

    LOG_INFO("CPU: guest thread launching (%s)", "");
    std::thread guest([&] {
        LOG_INFO("CPU: guest thread entered trampoline", "");
        kernel::installThreadTls();
        LOG_INFO("CPU: guest thread TLS installed", "");
        using GuestEntry = void (*)();
        reinterpret_cast<GuestEntry>(mod.entry)();
        LOG_INFO("CPU: guest entry returned naturally", "");
        // Guest returned without ExitProcess: natural completion.
        std::lock_guard<std::mutex> lk(m);
        guestDone = true;
        cv.notify_all();
    });

    // Supervisor wait: guest finished naturally, or an exit was requested
    // ( sceKernelExitProcess from any guest thread), or a crash was logged.
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] {
            return guestDone || kernelState.exitRequested()
                            || !result.crashReport.empty();
        });
    }
    if (guestDone && !kernelState.exitRequested()) {
        result.exitCode = 0;
        result.exitedViaGuest = true;
        guest.join();      // natural completion — safe to join
        LOG_INFO("CPU: guest entry returned (code 0)");
    } else {
        result.exitCode = kernelState.exitCode();
        result.exitedViaGuest = true;
        LOG_INFO("CPU: guest exit signaled (code %d)", result.exitCode);
        // The guest thread is parked inside the exit path (it must never
        // unwind guest frames under MSVC SEH) — join() would deadlock.
        // Detach it; the process is exiting and shutdownThreads detaches
        // every parked guest thread.
        guest.detach();
    }
    kernel::endSupervisorScope();
    return result;
}
#else
// ---------------------------------------------------------------------------
// POSIX hosting model (Phase 3, unified with the Windows shape): the guest
// main runs on a dedicated thread via kernel::runGuestMainCooperative —
//   * every thread that executes guest code carries the raw-syscall trap;
//   * engine threads (this supervisor) stay filter-free, which keeps host
//     tooling (ASan/LSan stop-the-world) safe;
//   * guest-initiated exits unwind the GUEST thread's own frames through its
//     per-thread exit env (no cross-thread longjmp anywhere).
// The supervisor waits for natural completion, an exit request, or a crash.
// ---------------------------------------------------------------------------
GuestRunResult runGuestEntry(const LoadedModule& mod, kernel::KernelState& kernelState) {
    GuestRunResult result;
    installCrashDiagnostics(&result.crashReport, mod.base, mod.imageSize);

    std::mutex m;
    std::condition_variable cv;
    bool guestDone = false;

    std::thread guest([&] {
        const i32 code = kernel::runGuestMainCooperative(
            reinterpret_cast<void*>(mod.entry), &guestDone);
        std::lock_guard<std::mutex> lk(m);
        result.exitCode = code;
        result.exitedViaGuest = true;
        cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] {
            return guestDone || kernelState.exitRequested()
                            || !result.crashReport.empty();
        });
        // Grace: let the guest main finish unwinding (it always terminates
        // promptly once done/exitRequested — it unwinds itself via its own
        // exit env). A compute-bound main that ignores the exit is joined
        // with a deadline, then detached and accounted (the pre-Phase-3
        // straggler path, loudly logged).
        if (!cv.wait_for(lk, std::chrono::milliseconds(200),
                         [&] { return guestDone; })) {
            LOG_WARN("CPU: guest main did not terminate within the join "
                     "grace — detaching (teardown straggler)");
            guest.detach();
            kernel::endSupervisorScope();
            return result;
        }
    }
    guest.join();
    if (!kernelState.exitRequested())
        LOG_INFO("CPU: guest exited naturally (code %d)", result.exitCode);
    else
        LOG_INFO("CPU: guest exited via sceKernelExitProcess(%d)", result.exitCode);
    return result;
}
#endif

} // namespace psb::cpu
