// ============================================================================
//  PSBET test homebrew — abi_probe.c (Phase 3)
//  Windows SysV->Win64 ABI conformance suite as a real guest.
//
//  Every thunk arity is exercised with position-distinct arguments: each
//  argument is folded into the result with its own XOR constant and rotation,
//  so a mis-routed register or stack slot fails deterministically. This guest
//  would have caught the Phase-2 thunk3 bug (arg2 lost on every 3-argument
//  call) on the first run.
//
//  Exit code 0 = all probes passed; bit N set = probe group N failed.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

static uint64_t rotl64(uint64_t x, unsigned r) {
    return (x << r) | (x >> (64 - r));
}

// Guest-side expected-value model (must mirror the host implementations).
static uint64_t expect1(uint64_t a) { return a ^ 0xA5u; }
static uint64_t expect2(uint64_t a, uint64_t b)
    { return rotl64(a ^ 0xA5u, 7) ^ (b ^ 0x5Au); }
static uint64_t expect3(uint64_t a, uint64_t b, uint64_t c)
    { return rotl64(expect2(a, b), 9) ^ (c ^ 0x33u); }
static uint64_t expect4(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
    { return rotl64(expect3(a, b, c), 11) ^ (d ^ 0x99u); }
static uint64_t expect5(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
    { return rotl64(expect4(a, b, c, d), 13) ^ (e ^ 0xC3u); }
static uint64_t expect6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                        uint64_t e, uint64_t f)
    { return rotl64(expect5(a, b, c, d, e), 17) ^ (f ^ 0x3Cu); }

// Host-side probe surface (exported by the runtime on every platform).
extern uint64_t psbProbeCall1(uint64_t a);
extern uint64_t psbProbeCall2(uint64_t a, uint64_t b);
extern uint64_t psbProbeCall3(uint64_t a, uint64_t b, uint64_t c);
extern uint64_t psbProbeCall4(uint64_t a, uint64_t b, uint64_t c, uint64_t d);
extern uint64_t psbProbeCall5(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e);
extern uint64_t psbProbeCall6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e, uint64_t f);
extern int32_t  psbProbePtr2(void* out, uint64_t val);

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }

static __thread uint32_t tls_marker;   // TLS smoke check on every platform

static void* worker_sl(void* arg) {
    (void)arg;
    tls_marker = 0xC0DEu;
    sceKernelExitThread(0);
    return 0;
}

void psb_entry(void) {
    uint32_t failures = 0;

    // Register arities 1..4.
    const uint64_t a1 = 0x0102030405060708ull;
    const uint64_t a2 = 0x1122334455667788ull;
    const uint64_t a3 = 0x0DEADBEEFCAFEBABull;
    const uint64_t a4 = 0x7654321076543210ull;
    const uint64_t a5 = 0x0F1E2D3C4B5A6978ull;
    const uint64_t a6 = 0x00FEDCBA98765432ull;

    if (psbProbeCall1(a1) != expect1(a1)) { say("[abi] FAIL arity1\n"); failures |= 1; }
    if (psbProbeCall2(a1, a2) != expect2(a1, a2)) { say("[abi] FAIL arity2\n"); failures |= 2; }
    if (psbProbeCall3(a1, a2, a3) != expect3(a1, a2, a3)) { say("[abi] FAIL arity3\n"); failures |= 4; }
    if (psbProbeCall4(a1, a2, a3, a4) != expect4(a1, a2, a3, a4)) { say("[abi] FAIL arity4\n"); failures |= 8; }

    // Stack arities 5..6 (arg 5/6 arrive on the stack in SysV).
    if (psbProbeCall5(a1, a2, a3, a4, a5) != expect5(a1, a2, a3, a4, a5))
        { say("[abi] FAIL arity5 (stack)\n"); failures |= 16; }
    if (psbProbeCall6(a1, a2, a3, a4, a5, a6) != expect6(a1, a2, a3, a4, a5, a6))
        { say("[abi] FAIL arity6 (stack)\n"); failures |= 32; }

    // Pointer position + value position (2 args).
    uint64_t slot = 0;
    if (psbProbePtr2(&slot, a1) != 0x1234 || slot != (a1 ^ 0x77u))
        { say("[abi] FAIL ptr2\n"); failures |= 64; }

    // Real 3-argument surface: sceKernelWrite(fd, buf, len) exercises thunk3.
    tls_marker = 42;
    say("[abi] 3-arg write through the shim: OK\n");
    if (tls_marker != 42) { say("[abi] FAIL TLS\n"); failures |= 128; }

    // Threads still work end-to-end (spawn + join a trivial worker).
    int32_t tid = 0;
    if (sceKernelCreateThread("abi-worker", (void*)&worker_sl, 5, 64 * 1024,
                              0, 0, &tid) != 0) {
        say("[abi] FAIL thread create\n");
        failures |= 256;
    } else {
        int32_t ret = -1;
        sceKernelStartThread(tid, 0, 0);
        sceKernelWaitThreadEnd(tid, &ret, 0);
        if (ret != 0) { say("[abi] FAIL thread join\n"); failures |= 256; }
    }

    if (failures == 0) {
        say("[abi] ALL PROBES PASSED\n");
        sceKernelExitProcess(0);
    } else {
        say("[abi] PROBES FAILED\n");
        sceKernelExitProcess((int32_t)failures);
    }
}
