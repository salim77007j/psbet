// ============================================================================
//  PSBET test homebrew — worker_exit.c
//  Phase-2 regression for flaw 1 (cross-thread longjmp on process exit):
//
//  main thread: waits on a semaphore (blocking kernel primitive).
//  worker thread: sleeps briefly, then calls sceKernelExitProcess(7).
//
//  Phase-1 behaviour: the worker longjmp'd on the MAIN thread's jmp_buf —
//  undefined behaviour (never observed only because Phase-1 guests exited
//  from the main thread). Phase-2 behaviour under test:
//    * the worker unwinds its OWN guest frames (thread exit env) and becomes
//      joinable — no parked-forever thread;
//    * the main thread's blocking wait wakes when the exit is requested;
//    * the supervisor delivers the worker's recorded exit code (7).
//  Expected process exit code: 7.
// ============================================================================
#include "psbet/psb_guest_api.h"

static int32_t sema = 0;

static int32_t worker_main(size_t arglen, void* argp) {
    (void)arglen; (void)argp;
    sceKernelUsleep(20000); // 20 ms: let main block on the semaphore first
    sceKernelExitProcess(7);
    return 0; // not reached (unwound by the exit)
}

void psb_entry(void) {
    if (sceKernelCreateSemaphore("exit", 0, 1, &sema) != 0)
        sceKernelExitProcess(1);

    int32_t tid = 0;
    if (sceKernelCreateThread("worker", (void*)&worker_main, 0, 64 * 1024, 0, 0, &tid) != 0)
        sceKernelExitProcess(1);
    if (sceKernelStartThread(tid, 0, 0) != 0)
        sceKernelExitProcess(1);

    // Blocks until woken by the worker's exit request.
    sceKernelWaitSemaphore(sema, 1, 0);

    // Exit already in flight (code 7 recorded). Reaching here is the
    // regression win; a plain exit() from main longjmps with the recorded
    // code — the process result must be 7, not 0.
    sceKernelExitProcess(0);
}
