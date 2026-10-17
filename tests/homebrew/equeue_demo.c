// ============================================================================
//  PSBET test homebrew — equeue_demo.c (Phase 3)
//  EQUEUE exercise: producer thread triggers events, main waits, cancel and
//  timeout paths are verified. Exit code 0 = all checks passed.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

static int32_t g_eq = 0;
static int32_t g_doneSema = 0;

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }
static void fail(const char* why) {
    say("[eq] CHECKS FAILED: ");
    say(why);
    say("\n");
    sceKernelExitProcess(1);
}

static int32_t producer(size_t arglen, void* argp) {
    (void)arglen; (void)argp;
    for (uint64_t i = 1; i <= 5; ++i) {
        sceKernelUsleep(2000);
        if (sceKernelTriggerEvent(g_eq, i, 0x10, i * 100) != 0)
            sceKernelExitThread(1);
    }
    sceKernelSignalSemaphore(g_doneSema, 1);
    sceKernelExitThread(0);
    return 0;
}

void psb_entry(void) {
    say("[eq] EQUEUE demo online\n");

    if (sceKernelCreateEqueue("events", &g_eq) != 0) fail("create");
    if (sceKernelCreateSemaphore("producer-done", 0, 1, &g_doneSema) != 0)
        fail("sema");

    int32_t tid = 0;
    if (sceKernelCreateThread("producer", (void*)&producer, 5, 64 * 1024,
                              0, 0, &tid) != 0) fail("thread");
    if (sceKernelStartThread(tid, 0, 0) != 0) fail("start");

    // Consume the 5 events in order.
    for (uint64_t i = 1; i <= 5; ++i) {
        PsbEqueueEvent ev;
        memset(&ev, 0, sizeof(ev));
        if (sceKernelWaitEqueue(g_eq, &ev, 0) != 0) fail("wait");
        if (ev.ident != i || ev.userData != i * 100 || ev.flags != 0x10) {
            say("[eq] FAIL: event payload mismatch\n");
            fail("payload");
        }
    }

    // Producer signals done; wait for it so teardown sees a joined thread.
    if (sceKernelWaitSemaphore(g_doneSema, 1, 0) != 0) fail("producer done");
    int32_t ret = -1;
    sceKernelWaitThreadEnd(tid, &ret, 0);
    if (ret != 0) fail("producer retval");

    // Timeout path: queue is empty now; a 50 ms wait must fail.
    PsbEqueueEvent ev;
    if (sceKernelWaitEqueue(g_eq, &ev, 50 * 1000) == 0) fail("timeout expected");

    // Cancel path: add two events with the same ident, cancel them, expect
    // the empty queue.
    sceKernelAddEvent(g_eq, 77, 1, 1);
    sceKernelAddEvent(g_eq, 77, 1, 2);
    sceKernelCancelEvent(g_eq, 77);
    if (sceKernelWaitEqueue(g_eq, &ev, 20 * 1000) == 0) fail("cancel expected");

    sceKernelDeleteEqueue(g_eq);
    say("[eq] ALL CHECKS PASSED\n");
    sceKernelExitProcess(0);
}
