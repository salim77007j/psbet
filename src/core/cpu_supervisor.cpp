// ============================================================================
//  PSBET — cpu_supervisor.cpp
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

namespace psb::cpu {

namespace {

jmp_buf     g_returnEnv;
std::string* g_reportSink = nullptr;
u64         g_modBase = 0;
u64         g_modSize = 0;
bool        g_diagnosticsInstalled = false;

void buildCrashReport(int sig, siginfo_t* info, void* /*ctx*/) {
    if (g_reportSink) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "Fatal signal %d at guest address 0x%llx "
                      "(module base 0x%llx, offset 0x%llx)\n",
                      sig,
                      static_cast<unsigned long long>(
                          reinterpret_cast<u64>(info->si_addr)),
                      static_cast<unsigned long long>(g_modBase),
                      static_cast<unsigned long long>(
                          reinterpret_cast<u64>(info->si_addr) - g_modBase));
        g_reportSink->assign(buf);
    }
    LOG_ERROR("CPU: fatal signal %d in guest (offset 0x%llx)",
              sig,
              static_cast<unsigned long long>(
                  reinterpret_cast<u64>(info->si_addr) - g_modBase));
    // Do not unwind through guest frames; exit with a distinctive code.
    std::_Exit(139);
}

} // namespace

void installCrashDiagnostics(std::string* reportSink, u64 moduleBase, u64 moduleSize) {
    g_reportSink = reportSink;
    g_modBase = moduleBase;
    g_modSize = moduleSize;

    struct sigaction sa{};
    sa.sa_sigaction = buildCrashReport;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    g_diagnosticsInstalled = true;
}

GuestRunResult runGuestEntry(const LoadedModule& mod, kernel::KernelState& kernelState) {
    GuestRunResult result;

    installCrashDiagnostics(&result.crashReport, mod.base, mod.imageSize);
    kernel::setProcessJmpBuf(&g_returnEnv);

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
    return result;
}

} // namespace psb::cpu
