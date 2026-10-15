// ============================================================================
//  PSBET test homebrew — hello.c
//  Exercises: dynamic import binding, TLS (__thread), threads, mutexes,
//  semaphores, timing and console I/O through the emulated libkernel.
//  Exit code 0 = all checks passed.
// ============================================================================
#include <string.h>
#include "psbet/psb_guest_api.h"

static __thread int32_t tls_counter = 0;      // general-dynamic TLS model
static int32_t shared_counter = 0;
static int32_t mutex_id = 0;
static int32_t sema_id = 0;

static void say(const char* s) {
    sceKernelWrite(1, s, strlen(s));
}

static void say_num(const char* prefix, int64_t v, const char* suffix) {
    char buf[128];
    // minimal int-to-string (avoid pulling more libc than needed)
    char digits[24];
    int di = 0;
    uint64_t uv = (v < 0) ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    if (uv == 0) digits[di++] = '0';
    while (uv > 0) { digits[di++] = (char)('0' + (uv % 10)); uv /= 10; }
    int bi = 0;
    const char* p = prefix;
    while (*p) buf[bi++] = *p++;
    if (v < 0) buf[bi++] = '-';
    while (di > 0) buf[bi++] = digits[--di];
    p = suffix;
    while (*p) buf[bi++] = *p++;
    sceKernelWrite(1, buf, (size_t)bi);
}

static int32_t worker_entry(size_t arglen, void* argp) {
    (void)arglen;
    int32_t worker_id = argp ? *(int32_t*)argp : 0;

    for (int i = 0; i < 1000; ++i) {
        sceKernelLockMutex(mutex_id, 0);
        shared_counter++;
        sceKernelUnlockMutex(mutex_id);
        tls_counter++;
        sceKernelWaitSemaphore(sema_id, 1, 0);   // handshake budget
        sceKernelSignalSemaphore(sema_id, 1);
        if (i == 0) sceKernelUsleep(1000);        // exercise sleep path once
    }

    say("[hello] worker ");
    say_num("", worker_id, " done: tls_counter=");
    say_num("", tls_counter, "\n");

    // TLS must be per-thread: exactly 1000 increments on this thread's copy.
    return (tls_counter == 1000) ? 0 : 1;
}

void psb_entry(void) {
    const uint64_t t0 = sceKernelGetProcessTime();

    say("[hello] PSBET guest ABI v1 online\n");
    say_num("[hello] runtime clock: ", (int64_t)sceKernelGetProcessTime(), " us\n");

    if (sceKernelCreateMutex("shared", 0, 0, &mutex_id) != 0) {
        say("[hello] FAIL: mutex create\n");
        sceKernelExitProcess(1);
    }
    if (sceKernelCreateSemaphore("handshake", 1, 4, &sema_id) != 0) {
        say("[hello] FAIL: semaphore create\n");
        sceKernelExitProcess(1);
    }

    int32_t tids[2];
    int32_t ids[2] = {1, 2};
    for (int i = 0; i < 2; ++i) {
        if (sceKernelCreateThread(i == 0 ? "worker-a" : "worker-b",
                                  (void*)&worker_entry, 5, 256 * 1024,
                                  0, 0, &tids[i]) != 0) {
            say("[hello] FAIL: thread create\n");
            sceKernelExitProcess(1);
        }
        if (sceKernelStartThread(tids[i], sizeof(int32_t), &ids[i]) != 0) {
            say("[hello] FAIL: thread start\n");
            sceKernelExitProcess(1);
        }
    }

    // Main thread's TLS copy must stay untouched (0).
    int32_t mainTlsOk = (tls_counter == 0);

    for (int i = 0; i < 2; ++i) {
        int32_t ret = -1;
        sceKernelWaitThreadEnd(tids[i], &ret, 0);
        if (ret != 0) {
            say("[hello] FAIL: worker returned nonzero\n");
            sceKernelExitProcess(1);
        }
    }

    const uint64_t elapsed = sceKernelGetProcessTime() - t0;
    say_num("[hello] 2 workers x 1000 locked increments = ", shared_counter,
            " shared increments\n");
    say_num("[hello] elapsed: ", (int64_t)elapsed, " us\n");

    const int32_t ok = (shared_counter == 2000) && mainTlsOk;
    say(ok ? "[hello] ALL CHECKS PASSED\n" : "[hello] CHECKS FAILED\n");
    sceKernelExitProcess(ok ? 0 : 1);
}
