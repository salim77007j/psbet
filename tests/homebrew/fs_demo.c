// ============================================================================
//  PSBET test homebrew — fs_demo.c (Phase 3)
//  Virtualized filesystem exercise: /data write+read+lseek+fstat passthrough
//  and (when an app0 image is mounted) read-only container access.
//  Exit code 0 = all checks passed.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }

static void fail(void) { say("[fs] CHECKS FAILED\n"); sceKernelExitProcess(1); }

void psb_entry(void) {
    say("[fs] VFS demo online\n");

    // --- /data: create, write, verify via fstat, read back -------------------
    // Open flags per GUEST_ABI v1.2 §8: RDWR=0x2, CREATE=0x40.
    int32_t fd = sceKernelOpen("/data/hello.txt", 0x2 | 0x40, 0);
    if (fd < 0) {
        say("[fs] FAIL: /data open(/hello.txt RDWR|CREATE)\n");
        fail();
    }
    const char* msg = "psbet vfs roundtrip";
    if (sceKernelWrite(fd, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        say("[fs] FAIL: write\n");
        fail();
    }
    PsbStat st;
    memset(&st, 0xAA, sizeof(st));
    if (sceKernelFstat(fd, &st) != 0 || st.size != strlen(msg) || st.isDir) {
        say("[fs] FAIL: fstat after write\n");
        fail();
    }
    sceKernelClose(fd);

    fd = sceKernelOpen("/data/hello.txt", 0x0 /*RDONLY*/, 0);
    if (fd < 0) { say("[fs] FAIL: reopen RDONLY\n"); fail(); }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (sceKernelRead(fd, buf, sizeof(buf)) != (int64_t)strlen(msg)
        || memcmp(buf, msg, strlen(msg)) != 0) {
        say("[fs] FAIL: read-back mismatch\n");
        fail();
    }
    // lseek: seek past the "psbet vfs " prefix (10 bytes) and read the tail.
    if (sceKernelLseek(fd, 10, 0 /*SET*/) != 10) { say("[fs] FAIL: lseek SET\n"); fail(); }
    memset(buf, 0, sizeof(buf));
    if (sceKernelRead(fd, buf, 9) != 9 || memcmp(buf, "roundtrip", 9) != 0) {
        say("[fs] FAIL: lseek+read tail\n");
        fail();
    }
    if (sceKernelLseek(fd, 0, 2 /*END*/) != (int64_t)strlen(msg)) {
        say("[fs] FAIL: lseek END\n");
        fail();
    }
    sceKernelClose(fd);

    // --- app0 (mounted image, read-only) --------------------------------------
    fd = sceKernelOpen("app0:/manifest.txt", 0, 0);
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        const int64_t n = sceKernelRead(fd, buf, sizeof(buf) - 1);
        sceKernelClose(fd);
        if (n <= 0 || strncmp(buf, "PSBET", 5) != 0) {
            say("[fs] FAIL: app0 manifest content\n");
            fail();
        }
        say("[fs] app0 manifest read OK\n");
        // Write to app0 must fail (read-only mount).
        fd = sceKernelOpen("app0:/manifest.txt", 0x1 /*WRONLY*/, 0);
        if (fd >= 0) { say("[fs] FAIL: app0 write allowed\n"); fail(); }
    } else {
        say("[fs] app0 not mounted this run — container checks skipped\n");
    }

    // --- sandbox boundary: host paths must NOT resolve ------------------------
    fd = sceKernelOpen("/etc/hostname", 0, 0);
    if (fd >= 0) {
        say("[fs] FAIL: host path escaped the sandbox\n");
        fail();
    }
    say("[fs] host-path escape refused as designed\n");

    say("[fs] ALL CHECKS PASSED\n");
    sceKernelExitProcess(0);
}
