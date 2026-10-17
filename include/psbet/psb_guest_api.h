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

// ---- Memory (v1.2: typed pools, page tables, GPU VM) -------------------------
// Prot bits (PSBET definition, identical across the memory surface):
#define PSB_PROT_READ   0x1
#define PSB_PROT_WRITE  0x2
#define PSB_PROT_EXEC   0x4
// Map flags: bit0 = fixed (addrOut is an in/out desired CPU VA; must equal
// the identity mapping arena+physAddr in v1.2 — see GUEST_ABI §5).
#define PSB_MAP_FIXED   0x1

int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd,
                                      size_t len, size_t alignment,
                                      int memoryType, int64_t* physAddrOut);
int32_t sceKernelFreeDirectMemory(int64_t physAddr, size_t len);
int32_t sceKernelMapDirectMemory(void** addrOut, size_t len, int prot, int flags,
                                 int64_t physAddr, size_t pageSize);
int32_t sceKernelMapNamedDirectMemory(void** addrOut, size_t len, int prot,
                                      int flags, int64_t physAddr,
                                      size_t pageSize, const char* name);
int32_t sceKernelGetDirectMemorySize(size_t* out);
// Anonymous guest mappings (fd-based mmap arrives with a real need).
int32_t sceKernelMmap(void** addrOut, size_t len, int prot, int flags,
                      int32_t fd, int64_t offset);
int32_t sceKernelMunmap(void* addr, size_t len);
// Query the mapping covering one address (Orbis-style).
int32_t sceKernelQueryMemoryProtection(void* addr, void** startOut,
                                       void** endOut, int32_t* protOut);

// ---- GPU virtual memory (v1.2) -------------------------------------------------
// The GPU owns a separate VA space: reserve an address window, then map
// physical (direct-memory) pages into it. Command packets may then reference
// the GPU VA (PBFF v1.1 DrawTrianglesGpu).
int32_t sceGnmReserveGpuAddress(uint64_t len, uint64_t alignment,
                                uint64_t fixedGpuVa, uint64_t* gpuVaOut);
int32_t sceGnmMapGpuAddress(uint64_t gpuVa, int64_t physAddr, uint64_t len,
                            uint32_t prot);
int32_t sceGnmUnmapGpuAddress(uint64_t gpuVa, uint64_t len);

// ---- Event queues (EQUEUE, v1.2) ------------------------------------------------
typedef struct PsbEqueueEvent {
    uint64_t ident;      // caller-chosen event id
    uint32_t flags;      // caller-chosen event kind
    uint64_t userData;   // opaque payload
} PsbEqueueEvent;

int32_t sceKernelCreateEqueue(const char* name, int32_t* out);
int32_t sceKernelDeleteEqueue(int32_t eqId);
int32_t sceKernelAddEvent(int32_t eqId, uint64_t ident, uint32_t flags,
                          uint64_t userData);
int32_t sceKernelTriggerEvent(int32_t eqId, uint64_t ident, uint32_t flags,
                              uint64_t userData);
int32_t sceKernelWaitEqueue(int32_t eqId, PsbEqueueEvent* eventOut,
                            uint64_t timeoutUs);
int32_t sceKernelCancelEvent(int32_t eqId, uint64_t ident);

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

// ---- I/O (v1.2: virtualized filesystem — see GUEST_ABI §8) ----------------------
int64_t sceKernelWrite(int32_t fd, const void* buf, size_t n);
int64_t sceKernelRead(int32_t fd, void* buf, size_t n);
int32_t sceKernelOpen(const char* path, int32_t flags, uint32_t mode);
int32_t sceKernelClose(int32_t fd);
int64_t sceKernelLseek(int32_t fd, int64_t offset, int32_t whence);
typedef struct PsbStat {
    uint64_t size;
    uint32_t isDir;
    uint32_t readOnly;
} PsbStat;
int32_t sceKernelFstat(int32_t fd, PsbStat* out);

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
