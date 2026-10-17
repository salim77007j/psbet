// ============================================================================
//  PSBET — net_stubs.cpp (Phase 4) — logged network stubs (no connectivity).
// ============================================================================
#include "core/net_stubs.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "core/log.h"

using psb::u64;
using psb::i32;

namespace {

std::atomic<u64> g_calls{0};
std::atomic<u64> g_refused{0};
std::atomic<u64> g_logged{0};
constexpr u64 kLogFullCalls = 8;    // first N calls logged with full args

} // namespace

extern "C" {

int32_t sceNetInit(uint64_t flags) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetInit refused (no network in PSBET) — flags=0x%llx",
                 static_cast<unsigned long long>(flags));
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrUnavailable;
}

int32_t sceNetTerm(void) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetTerm refused (no network in PSBET)");
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetSocket(const char* name, uint32_t domain, uint32_t type, uint32_t proto) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetSocket refused — name=%s domain=%u type=%u proto=%u",
                 name ? name : "(null)", domain, type, proto);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrUnavailable;
}

int32_t sceNetConnect(int32_t socket, const void* addr, uint32_t addrLen) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetConnect refused — sock=%d addr=%p len=%u",
                 socket, addr, addrLen);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetBind(int32_t socket, const void* addr, uint32_t addrLen) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetBind refused — sock=%d addr=%p len=%u",
                 socket, addr, addrLen);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetListen(int32_t socket, int32_t backlog) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetListen refused — sock=%d backlog=%d",
                 socket, backlog);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetAccept(int32_t socket, void* addrOut, uint32_t* addrLenInOut) {
    (void)addrLenInOut;
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetAccept refused — sock=%d addrOut=%p",
                 socket, addrOut);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetSend(int32_t socket, const void* buf, uint32_t len, int32_t flags) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetSend refused — sock=%d buf=%p len=%u flags=%d",
                 socket, buf, len, flags);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetRecv(int32_t socket, void* buf, uint32_t len, int32_t flags) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetRecv refused — sock=%d buf=%p len=%u flags=%d",
                 socket, buf, len, flags);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetCloseSocket(int32_t socket) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetCloseSocket refused — sock=%d", socket);
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetCtlInit(void) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetCtlInit refused (no network in PSBET)");
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrUnavailable;
}

int32_t sceNetCtlTerm(void) {
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetCtlTerm refused (no net control state)");
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    return psb::net::kErrNotInitialized;
}

int32_t sceNetCtlGetState(int32_t* stateOut) {
    // Honest and deterministic: PSBET reports the pad-less truth — offline.
    const u64 n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFullCalls) {
        LOG_WARN("net-stub: sceNetCtlGetState — reporting disconnected");
        g_logged.fetch_add(1, std::memory_order_relaxed);
    }
    if (stateOut) *stateOut = 0;
    return 0;
}

} // extern "C"

namespace psb::net {

Stats stats() {
    Stats s;
    s.calls = g_calls.load(std::memory_order_relaxed);
    s.refused = g_refused.load(std::memory_order_relaxed);
    s.logged = g_logged.load(std::memory_order_relaxed);
    return s;
}

void resetCounters() {
    g_calls.store(0, std::memory_order_relaxed);
    g_refused.store(0, std::memory_order_relaxed);
    g_logged.store(0, std::memory_order_relaxed);
}

} // namespace psb::net

#if defined(_WIN32)
// Windows SysV->Win64 trampoline targets (w64_thunks.asm).
extern "C" {
int32_t psbXNetInit(uint64_t a)                                     { return sceNetInit(a); }
int32_t psbXNetTerm(void)                                           { return sceNetTerm(); }
int32_t psbXNetSocket(const char* a, uint32_t b, uint32_t c, uint32_t d)
    { return sceNetSocket(a, b, c, d); }
int32_t psbXNetConnect(int32_t a, const void* b, uint32_t c)         { return sceNetConnect(a, b, c); }
int32_t psbXNetBind(int32_t a, const void* b, uint32_t c)            { return sceNetBind(a, b, c); }
int32_t psbXNetListen(int32_t a, int32_t b)                          { return sceNetListen(a, b); }
int32_t psbXNetAccept(int32_t a, void* b, uint32_t* c)               { return sceNetAccept(a, b, c); }
int32_t psbXNetSend(int32_t a, const void* b, uint32_t c, int32_t d) { return sceNetSend(a, b, c, d); }
int32_t psbXNetRecv(int32_t a, void* b, uint32_t c, int32_t d)       { return sceNetRecv(a, b, c, d); }
int32_t psbXNetCloseSocket(int32_t a)                                { return sceNetCloseSocket(a); }
int32_t psbXNetCtlInit(void)                                         { return sceNetCtlInit(); }
int32_t psbXNetCtlTerm(void)                                         { return sceNetCtlTerm(); }
int32_t psbXNetCtlGetState(int32_t* a)                               { return sceNetCtlGetState(a); }
}
#endif
