// ============================================================================
//  PSBET — psb_guest_api.h
//  The guest-facing surface of PSBET's runtime (guest ABI v1).
//
//  This is what homebrew authors compile against. It is a clean-room
//  definition: PS5-style naming for familiarity, original simplified
//  signatures, zero proprietary material. Build with:
//
//    gcc -shared -fPIC -nostartfiles -Wl,-e,psb_entry -o game.elf game.c
//
//  (Full contract: docs/GUEST_ABI.md)
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Required guest entry point --------------------------------------------
// Called once on the main thread after the image is mapped, relocated and
// initialized. Return or call sceKernelExitProcess to end the process.
void psb_entry(void);

// ---- Memory -----------------------------------------------------------------
int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd,
                                      size_t len, size_t alignment,
                                      int memoryType, int64_t* physAddrOut);
int32_t sceKernelMapDirectMemory(void** addrOut, size_t len, int prot, int flags,
                                 int64_t physAddr, size_t pageSize);
int32_t sceKernelGetDirectMemorySize(size_t* out);

// ---- Time (microsecond / nanosecond clocks) ----------------------------------
uint64_t sceKernelGetProcessTime(void);          // microseconds since start
uint64_t sceKernelGetProcessTimeInTick(void);    // PSBET ticks (1 tick = 1 ns)
int32_t  sceKernelUsleep(uint64_t microseconds);
int32_t  sceKernelNanosleep(uint64_t seconds, uint64_t nanoseconds);
int32_t  sceKernelSleep(uint64_t seconds);

// ---- Threads ------------------------------------------------------------------
typedef int32_t (*PsbThreadEntry)(size_t arglen, void* argp);

int32_t sceKernelCreateThread(const char* name, void* entry, int32_t initPriority,
                              size_t stackSize, uint64_t attr,
                              uint64_t cpuAffinityMask, int32_t* out);
int32_t sceKernelStartThread(int32_t tid, size_t arglen, void* argp);
int32_t sceKernelWaitThreadEnd(int32_t tid, int32_t* retVal, uint64_t* timeoutUs);
void    sceKernelExitThread(int32_t retVal);
void    sceKernelExitProcess(int32_t status);
int32_t sceKernelGetThreadId(void);

// ---- Synchronization ------------------------------------------------------------
int32_t sceKernelCreateMutex(const char* name, uint32_t attr, int32_t initLocked,
                             int32_t* out);
int32_t sceKernelLockMutex(int32_t mutexId, uint64_t timeoutUs);
int32_t sceKernelUnlockMutex(int32_t mutexId);
int32_t sceKernelCreateSemaphore(const char* name, int32_t initCount,
                                 int32_t maxCount, int32_t* out);
int32_t sceKernelWaitSemaphore(int32_t semaId, int32_t needCount, uint64_t timeoutUs);
int32_t sceKernelSignalSemaphore(int32_t semaId, int32_t signalCount);
int32_t sceKernelCreateCond(const char* name, uint32_t attr, int32_t* out);
int32_t sceKernelWaitCond(int32_t condId, int32_t mutexId, uint64_t timeoutUs);
int32_t sceKernelSignalCond(int32_t condId);
int32_t sceKernelSignalCondAll(int32_t condId);

// ---- I/O --------------------------------------------------------------------------
int64_t sceKernelWrite(int32_t fd, const void* buf, size_t n);
int64_t sceKernelRead(int32_t fd, void* buf, size_t n);
int32_t sceKernelOpen(const char* path, int32_t flags, uint32_t mode);
int32_t sceKernelClose(int32_t fd);

// ---- GPU submission -----------------------------------------------------------------
// Submits display command buffers (PBFF v1, see gpu/cmd_stream.h in the
// runtime tree). Count/arrays mirror the Orbis-style submit shape.
int32_t sceGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                   uint32_t* dcbSizesInBytes,
                                   void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                   uint32_t flags);

#ifdef __cplusplus
}
#endif
