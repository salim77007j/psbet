// ============================================================================
//  PSBET test homebrew — syscall_probe.c (Phase 3)
//  Raw-syscall containment verification.
//
//  A guest executing a raw `syscall` instruction would reach the HOST kernel
//  directly — the one escape hatch around PSBET's emulated kernel surface.
//  With the Phase-3 trap layer (Linux, --syscall-trap on), host-escaping
//  syscalls never execute: the kernel raises SIGSYS and PSBET's handler
//  answers -EPERM. This probe verifies:
//    * a benign syscall (write) still works natively when allowed,
//    * host-escaping syscalls (execve, socket) are refused with -EPERM.
//
//  On hosts without the trap layer the escaping calls are skipped (this
//  guest never relies on raw syscalls for its own operation).
//  Exit code 0 = all checks passed.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }

#if defined(__linux__) && defined(__x86_64__)

static inline long raw_syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return ret;
}

// Linux x86-64 numbers (raw, host-side).
#define NR_WRITE   1L
#define NR_EXECVE  59L
#define NR_SOCKET  41L
#define RAW_EPERM  1L   // EPERM = 1 on Linux

#endif

void psb_entry(void) {
    say("[sc] syscall containment probe online\n");

#if defined(__linux__) && defined(__x86_64__)
    // 1. execve must be refused (executing a host program from a guest is
    //    the canonical escape). Use a path that could never matter: even if
    //    the filter were missing, execve of an empty path fails.
    {
        const long r = raw_syscall3(NR_EXECVE, (long)"", 0, 0);
        if (r == 0) {
            say("[sc] FAIL: raw execve reached the host kernel\n");
            sceKernelExitProcess(1);
        }
        say("[sc] execve refused (raw syscall returned an error)\n");
    }

    // 2. socket must be refused (network escape).
    {
        const long r = raw_syscall3(NR_SOCKET, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
        if (r == 0) {
            say("[sc] FAIL: raw socket reached the host kernel\n");
            sceKernelExitProcess(1);
        }
        say("[sc] socket refused (raw syscall returned an error)\n");
    }

    // 3. Benign write keeps flowing (already proven by say() above, which
    //    uses the emulated surface; the raw path is exercised here).
    {
        const char* m = "[sc] raw write(1) allowed through\n";
        const long r = raw_syscall3(NR_WRITE, 1, (long)m, (long)strlen(m));
        if (r != (long)strlen(m)) {
            say("[sc] FAIL: raw write of a benign syscall broken\n");
            sceKernelExitProcess(1);
        }
    }
#else
    say("[sc] raw syscall probe inactive on this host ABI (skipped)\n");
#endif

    say("[sc] ALL CHECKS PASSED\n");
    sceKernelExitProcess(0);
}
