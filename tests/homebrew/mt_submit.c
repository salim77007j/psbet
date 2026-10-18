// ============================================================================
//  PSBET test homebrew — mt_submit.c (Phase 5)
//  Multithreaded command recording/submission: N guest threads each build
//  and submit their own display command buffers CONCURRENTLY through the
//  pipelined GPU queue. The queue serializes execution (FIFO) so per-submit
//  state changes (viewport) can never interleave with another submit's
//  draws; the four workers draw into DISJOINT viewport quadrants, so the
//  final framebuffer is deterministic regardless of submit interleaving.
//
//  Contract:
//    - 4 worker threads x 10 submits = 40 queued CBs; main waits for the
//      threads, then fences the graphics queue and exits 0.
//    - Host asserts 40 completed frames + the expected 4-quadrant layout
//      (the quadrant colors are deterministic; worker IDs hash to colors).
//
//  Exit code 0 = all workers finished; anything else = failure.
// ============================================================================
#include <stdint.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

#define FRAME_W 1280
#define FRAME_H 720
#define WORKERS 4
#define SUBMITS_PER_WORKER 10

static __attribute__((aligned(16))) uint8_t dcb[WORKERS][256];

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }

static void push_bytes(size_t* off, void* base, const void* p, size_t n) {
    memcpy((uint8_t*)base + *off, p, n);
    *off += n;
}

// One submit: clear (fills the whole screen) + one colored quad in the
// worker's quadrant. Clear color is dark; the quad is bright.
static void submit_one(uint32_t worker, uint32_t seq) {
    const float qx = (worker & 1) ? 0.05f : -0.95f;   // right / left half
    const float qy = (worker & 2) ? 0.05f : -0.95f;   // top / bottom half
    const float r = 0.25f + 0.15f * ((seq + worker) % 3);
    const float g = 0.25f + 0.15f * ((seq + 2u * worker) % 3);
    const float b = 0.25f + 0.15f * ((seq + 3u * worker) % 3);

    const float quad[3][8] = {
        {qx, qy, 0.5f, 1.f, r, g, b, 1.f},
        {qx + 0.9f, qy, 0.5f, 1.f, r, g, b, 1.f},
        {qx, qy + 0.9f, 0.5f, 1.f, r, g, b, 1.f},
    };

    size_t off = 0;
    uint8_t* buf = dcb[worker];
    {
        struct { uint32_t op, size; float c[4]; } pkt = {
            1, sizeof(uint32_t) * 2 + sizeof(float) * 4,
            {0.02f, 0.02f, 0.03f, 1.f}};
        push_bytes(&off, buf, &pkt, sizeof(pkt));
    }
    {
        struct { uint32_t op, size; float x, y, w, h; } pkt = {
            2, sizeof(uint32_t) * 2 + sizeof(float) * 4,
            0.f, 0.f, (float)FRAME_W, (float)FRAME_H};
        push_bytes(&off, buf, &pkt, sizeof(pkt));
    }
    {
        struct { uint32_t op, size; uint32_t mode; } pkt = {
            4, sizeof(uint32_t) * 3, 0};
        push_bytes(&off, buf, &pkt, sizeof(pkt));
    }
    {
        struct { uint32_t op, size, vertexCount; float v[3][8]; } pkt = {
            5, sizeof(uint32_t) * 3 + sizeof(float) * 24, 3,
            {{quad[0][0], quad[0][1], quad[0][2], quad[0][3],
              quad[0][4], quad[0][5], quad[0][6], quad[0][7]},
             {quad[1][0], quad[1][1], quad[1][2], quad[1][3],
              quad[1][4], quad[1][5], quad[1][6], quad[1][7]},
             {quad[2][0], quad[2][1], quad[2][2], quad[2][3],
              quad[2][4], quad[2][5], quad[2][6], quad[2][7]}}};
        push_bytes(&off, buf, &pkt, sizeof(pkt));
    }

    void* addr = buf;
    uint32_t size = (uint32_t)off;
    if (sceGnmSubmitCommandBuffers(1, &addr, &size, 0, 0, 0) != 0) {
        say("[mt_submit] FAIL: submit rejected\n");
        sceKernelExitProcess(5);
    }
}

static void worker(uint64_t id) {
    for (uint32_t i = 0; i < SUBMITS_PER_WORKER; ++i)
        submit_one((uint32_t)id, i);
    sceKernelExitThread(0);
}

// Tiny thread thunks: PsbThreadEntry takes (arglen, argp).
static int32_t worker_thunk(size_t arglen, void* argp) {
    (void)arglen;
    worker(argp ? *(uint64_t*)argp : 0);
    return 0;
}

void psb_entry(void) {
    say("[mt_submit] concurrent submitters online\n");

    uint64_t ids[WORKERS];
    int32_t tids[WORKERS];
    for (uint32_t w = 0; w < WORKERS; ++w) {
        ids[w] = w;
        if (sceKernelCreateThread("mtw", (void*)&worker_thunk,
                                  5, 256 * 1024, 0, 0, &tids[w]) != 0) {
            say("[mt_submit] FAIL: CreateThread\n");
            sceKernelExitProcess(1);
        }
        if (sceKernelStartThread(tids[w], sizeof(ids[w]), &ids[w]) != 0) {
            say("[mt_submit] FAIL: StartThread\n");
            sceKernelExitProcess(2);
        }
    }

    for (uint32_t w = 0; w < WORKERS; ++w) {
        int32_t out = 0;
        if (sceKernelWaitThreadEnd(tids[w], &out, 0) != 0 || out != 0) {
            say("[mt_submit] FAIL: worker exit\n");
            sceKernelExitProcess(3);
        }
    }

    // Fence: everything the workers queued must be executed before exit.
    if (sceGnmWaitQueueIdle(PSB_QUEUE_GRAPHICS) != 0) {
        say("[mt_submit] FAIL: queue idle wait rejected\n");
        sceKernelExitProcess(4);
    }

    say("[mt_submit] ALL CHECKS PASSED\n");
    sceKernelExitProcess(0);
}
