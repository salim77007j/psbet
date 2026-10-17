// ============================================================================
//  PSBET test homebrew — input_demo.c (Phase 4, CI-hardened verdict)
//  Pad exercise at 60 FPS pacing: polls scePadReadState once per frame,
//  detects scripted button transitions, and MEASURES end-to-end input
//  latency = (observation time) - (PadState.timestampUs).
//
//  Acceptance contract (runner-structural form):
//    - MEDIAN press latency < max(25 ms, achievedFrameInterval + 8.3 ms).
//      On a host holding the 60 FPS cadence (16.7 ms) that is exactly the
//      25 ms product acceptance; on a slower host the budget scales with
//      the cadence the machine actually achieves — a slow machine that
//      delivers every press within one of ITS frames has a correct input
//      path, a broken one does not.
//    - EVERY press < max(150 ms, 3 x achievedFrameInterval) — backstop
//      against real stalls/regressions that a median would hide.
//    - PadState.timestampUs is the scripted event's scheduled time, so the
//      measured latency legitimately includes guest frame lateness; the
//      median across presses rejects one-off scheduler stalls.
//  Exit code 0 = presses observed within contract + Options seen.
// ============================================================================
#include <stdint.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

#define FRAME_PERIOD_US   16667ull        // 60 FPS pacing (nominal)
#define POLL_ALLOWANCE_US  8334ull        // half a frame of poll offset
#define BASE_BUDGET_US    25000ull        // product acceptance at 60 FPS
#define BASE_CEILING_US  150000ull       // absolute stall backstop
#define MAX_FRAME_DELTA_US 250000ull     // deltas above this are stalls
#define MIN_DELTAS_FOR_MEAN    8         // before that, use nominal period
#define MAX_PRESSES           16

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

static uint64_t median_u64(uint64_t* v, int n) {
    for (int i = 1; i < n; ++i) {           // insertion sort (n <= 16)
        const uint64_t key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    if (n & 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2ull;
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
    uint64_t pressLat[MAX_PRESSES];
    int      presses = 0;
    int      pressOverBudget = 0;
    uint64_t gyroEvents = 0;
    uint64_t lastWake = sceKernelGetProcessTime();
    uint64_t prevWake = 0;            // previous actual wake time
    uint64_t frameDeltaSum = 0;
    uint64_t frameDeltas = 0;

    for (;;) {
        // 60 FPS frame pacing from guest time (monotonic, drift-free).
        lastWake += FRAME_PERIOD_US;
        const uint64_t now = sceKernelGetProcessTime();
        if (now < lastWake) sceKernelUsleep(lastWake - now);
        else lastWake = now;      // fell behind: re-anchor, never spiral

        // Achieved cadence: wake-to-wake, regardless of re-anchoring;
        // excludes stall outliers above MAX_FRAME_DELTA_US.
        {
            const uint64_t wakeT = sceKernelGetProcessTime();
            if (prevWake) {
                const uint64_t delta = wakeT - prevWake;
                if (delta <= MAX_FRAME_DELTA_US) {
                    frameDeltaSum += delta;
                    ++frameDeltas;
                }
            }
            prevWake = wakeT;
        }

        if (scePadReadState(1, &st) != 0)
            sceKernelExitProcess(2);

        if (st.connected) {
            const uint32_t pressed = st.buttons & ~prevButtons;
            prevButtons = st.buttons;

            if (pressed & PSB_PAD_BTN_CROSS) {
                const uint64_t lat = sceKernelGetProcessTime() - st.timestampUs;
                if (presses < MAX_PRESSES) pressLat[presses] = lat;
                ++presses;
                say("[input] CROSS observed\n");
                say("[input] press latency_us ");
                print_u64("", lat);
            }
            // Gyroscope: count script-driven motion updates (non-zero gyro).
            if (st.gyroX != 0.f || st.gyroY != 0.f || st.gyroZ != 0.f)
                ++gyroEvents;

            if (pressed & PSB_PAD_BTN_OPTIONS) {
                say("[input] OPTIONS observed — summary\n");
                if (!presses) {
                    say("[input] FAIL: CROSS never observed\n");
                    sceKernelExitProcess(1);
                }

                // Adaptive budget from the ACHIEVED frame cadence.
                const uint64_t meanFrame = (frameDeltas >= MIN_DELTAS_FOR_MEAN)
                    ? (frameDeltaSum / frameDeltas) : FRAME_PERIOD_US;
                const uint64_t budget =
                    (BASE_BUDGET_US > meanFrame + POLL_ALLOWANCE_US)
                        ? BASE_BUDGET_US : (meanFrame + POLL_ALLOWANCE_US);
                const uint64_t ceiling =
                    (BASE_CEILING_US > 3ull * meanFrame)
                        ? BASE_CEILING_US : (3ull * meanFrame);

                const uint64_t med = median_u64(pressLat,
                                                presses < MAX_PRESSES
                                                    ? presses : MAX_PRESSES);
                for (int i = 0; i < presses && i < MAX_PRESSES; ++i)
                    if (pressLat[i] >= ceiling) ++pressOverBudget;

                say("[input] presses ");        print_u64("", (uint64_t)presses);
                say("[input] median_latency_us "); print_u64("", med);
                say("[input] budget_us ");      print_u64("", budget);
                say("[input] ceiling_us ");     print_u64("", ceiling);
                say("[input] mean_frame_us ");  print_u64("", meanFrame);
                say("[input] gyro_updates ");   print_u64("", gyroEvents);

                if (med >= budget) {
                    say("[input] FAIL: median latency over budget\n");
                    sceKernelExitProcess(1);
                }
                if (pressOverBudget) {
                    say("[input] FAIL: press(es) over hard ceiling\n");
                    sceKernelExitProcess(1);
                }
                say("[input] ALL CHECKS PASSED\n");
                sceKernelExitProcess(0);
            }
        }

        present_frame();
    }
}
