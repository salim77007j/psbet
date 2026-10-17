// ============================================================================
//  PSBET test homebrew — net_probe.c (Phase 4)
//  Network surface exercise: every stub must return its DOCUMENTED error
//  code (GUEST_ABI §11) — never connect, never hang, never leak a host
//  resource. Exit code 0 = every documented code observed.
// ============================================================================
#include <stdint.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }
static void fail(const char* why) {
    say("[net] CHECKS FAILED: ");
    say(why);
    say("\n");
    sceKernelExitProcess(1);
}

void psb_entry(void) {
    say("[net] surface probe online\n");

    if (sceNetInit(0) != PSB_NET_ERR_UNAVAILABLE) fail("sceNetInit");
    if (sceNetSocket("probe", 2, 1, 6) != PSB_NET_ERR_UNAVAILABLE)
        fail("sceNetSocket");
    if (sceNetConnect(3, 0, 0) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetConnect");
    if (sceNetBind(3, 0, 0) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetBind");
    if (sceNetListen(3, 4) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetListen");
    if (sceNetAccept(3, 0, 0) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetAccept");
    if (sceNetSend(3, "x", 1, 0) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetSend");
    uint8_t rb[8];
    if (sceNetRecv(3, rb, sizeof(rb), 0) != PSB_NET_ERR_NOT_INITIALIZED)
        fail("sceNetRecv");
    if (sceNetCloseSocket(3) != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetCloseSocket");
    if (sceNetCtlInit() != PSB_NET_ERR_UNAVAILABLE) fail("sceNetCtlInit");
    if (sceNetTerm() != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetTerm");
    if (sceNetCtlTerm() != PSB_NET_ERR_NOT_INITIALIZED) fail("sceNetCtlTerm");

    int32_t state = -1;
    if (sceNetCtlGetState(&state) != 0) fail("sceNetCtlGetState rc");
    if (state != 0) fail("sceNetCtlGetState state (must be disconnected)");

    say("[net] ALL CHECKS PASSED (every call refused with documented codes)\n");
    sceKernelExitProcess(0);
}
