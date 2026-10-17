// ============================================================================
//  PSBET — net_stubs.h (Phase 4)
//  Network surface: fully stubbed with logging. PSBET grants guests NO
//  network access — every call is logged (rate-limited) and returns a
//  documented error code (GUEST_ABI §11). Nothing here opens sockets,
//  resolves names, or touches the host network stack — by design, verifiable
//  by audit.
// ============================================================================
#pragma once

#include "psbet/common.h"

// Guest-visible surface (extern "C", registered in the kernel export table).
extern "C" {
int32_t sceNetInit(uint64_t flags);
int32_t sceNetTerm(void);
int32_t sceNetSocket(const char* name, uint32_t domain, uint32_t type,
                     uint32_t proto);
int32_t sceNetConnect(int32_t socket, const void* addr, uint32_t addrLen);
int32_t sceNetBind(int32_t socket, const void* addr, uint32_t addrLen);
int32_t sceNetListen(int32_t socket, int32_t backlog);
int32_t sceNetAccept(int32_t socket, void* addrOut, uint32_t* addrLenInOut);
int32_t sceNetSend(int32_t socket, const void* buf, uint32_t len, int32_t flags);
int32_t sceNetRecv(int32_t socket, void* buf, uint32_t len, int32_t flags);
int32_t sceNetCloseSocket(int32_t socket);
int32_t sceNetCtlInit(void);
int32_t sceNetCtlTerm(void);
int32_t sceNetCtlGetState(int32_t* stateOut);   // 0 = disconnected (always)
}

namespace psb::net {

// PSBET net error codes (guest ABI v1.3, GUEST_ABI §11).
constexpr i32 kErrNotInitialized = -0x4E01;  // surface not initialized
constexpr i32 kErrUnavailable    = -0x4E02;  // no network in this runtime
constexpr i32 kErrInvalidArg     = -0x4E03;

struct Stats {
    u64 calls = 0;          // total stub invocations this run
    u64 refused  = 0;       // calls refused with an error code
    u64 logged   = 0;       // calls fully logged
};
Stats stats();
void resetCounters();       // engine-side, between runs

} // namespace psb::net
