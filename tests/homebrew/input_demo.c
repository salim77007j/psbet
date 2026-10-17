// ============================================================================
//  PSBET test homebrew — input_demo.c (Phase 4)
//  Pad exercise at 60 FPS pacing: polls scePadReadState once per frame,
//  detects scripted button transitions, and MEASURES end-to-end input
//  latency = (observation time) - (PadState.timestampUs). The acceptance
//  contract is < 25 ms at 60 FPS pacing; the guest fails itself (exit 1)
//  when any observed press exceeds it.
//  Exit code 0 = press observed within budget + Options seen.
// ============================================================================
#include <stdint.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

#define FRAME_PERIOD_US  16667ull        // 60 FPS pacing
#define LATENCY_BUDGET_US 25000ull       // acceptance: < 25 ms

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }

static void print_u64(const char* tag, uint64_t v) {
    char tmp[64];
    const char* digits = "0123456789";
    int n = 0;
    char rev[24];
    if (!v) rev[n++] = '0';
    while (v) { rev[n++] = digits[v % 10]; v /= 10; }
    int m = 0;
    while (m < 63 && tag[m]) { tmp[m] = tag[m]; ++m; }
    while (n) { tmp[m++] = rev[--n]; }
    tmp[m++] = '\n';
    sceKernelWrite(1, tmp, m);
}

static __attribute__((aligned(16))) uint8_t dcb[32];
static void present_frame(void) {
    uint32_t* w = (uint32_t*)dcb;
    float*    c = (float*)(dcb + 8);
    w[0] = 1;                      // CmdOp::Clear
    w[1] = 24;
    c[0] = 0.02f; c[1] = 0.10f; c[2] = 0.05f; c[3] = 1.f;
    void* addr = dcb;
    uint32_t size = 24;
    sceGnmSubmitCommandBuffers(1, &addr, &size, 0, 0, 0);
}

void psb_entry(void) {
    say("[input] pad demo online\n");

    if (scePadOpen(0, 0, 0, 0) != 1)
        sceKernelExitProcess(2);

    PsbPadControllerInfo info;
    memset(&info, 0, sizeof(info));
    if (scePadGetControllerInformation(1, &info) != 0)
        sceKernelExitProcess(2);

    PsbPadState st;
    memset(&st, 0, sizeof(st));
    uint32_t prevButtons = 0;
    int crossObserved = 0;
    uint64_t crossLatencyUs = 0;
    uint64_t gyroEvents = 0;
    uint64_t lastWake = sceKernelGetProcessTime();

    for (;;) {
        // 60 FPS frame pacing from guest time (monotonic, drift-free).
        lastWake += FRAME_PERIOD_US;
        const uint64_t now = sceKernelGetProcessTime();
        if (now < lastWake) sceKernelUsleep(lastWake - now);
        else lastWake = now;      // fell behind: re-anchor, never spiral

        if (scePadReadState(1, &st) != 0)
            sceKernelExitProcess(2);

        if (st.connected) {
            const uint32_t pressed = st.buttons & ~prevButtons;
            prevButtons = st.buttons;

            if (pressed & PSB_PAD_BTN_CROSS) {
                crossLatencyUs = sceKernelGetProcessTime() - st.timestampUs;
                crossObserved = 1;
                say("[input] CROSS observed\n");
                say("[input] latency_us ");
                print_u64("", crossLatencyUs);
                if (crossLatencyUs >= LATENCY_BUDGET_US) {
                    say("[input] FAIL: latency over 25 ms budget\n");
                    sceKernelExitProcess(1);
                }
            }
            // Gyroscope: count script-driven motion updates (non-zero gyro).
            if (st.gyroX != 0.f || st.gyroY != 0.f || st.gyroZ != 0.f)
                ++gyroEvents;

            if (pressed & PSB_PAD_BTN_OPTIONS) {
                say("[input] OPTIONS observed — summary\n");
                if (!crossObserved) {
                    say("[input] FAIL: CROSS never observed\n");
                    sceKernelExitProcess(1);
                }
                say("[input] gyro_updates ");
                print_u64("", gyroEvents);
                say("[input] ALL CHECKS PASSED\n");
                sceKernelExitProcess(0);
            }
        }

        present_frame();
    }
}
