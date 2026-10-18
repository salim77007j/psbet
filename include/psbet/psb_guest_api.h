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

// ---- Time (microsecond / nanosecond clocks; v1.3 adds the counter API) ------
uint64_t sceKernelGetProcessTime(void);          // microseconds since start
uint64_t sceKernelGetProcessTimeInTick(void);    // PSBET ticks (1 tick = 1 ns)
uint64_t sceKernelGetProcessTimeCounter(void);   // counter ticks (1 tick = 1 ns)
uint64_t sceKernelGetProcessTimeCounterFrequency(void); // fixed: 1e9 Hz
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
// v1.4: submissions are ASYNCHRONOUS (console semantics) — the call returns
// when the buffers are accepted by the GPU queue, not when they complete.
// The host reads GPU-visible memory at execution time; guests that mutate
// submitted data must multi-buffer or fence. See docs/GUEST_ABI.md §16.
int32_t sceGnmSubmitCommandBuffers(uint32_t count, void** dcbAddrs,
                                   uint32_t* dcbSizesInBytes,
                                   void** ccbAddrs, uint32_t* ccbSizesInBytes,
                                   uint32_t flags);

// v1.4 — compute queue (docs/GUEST_ABI.md §16).
// sceGnmSubmitCompute submits command buffers to the compute queue (data-
// parallel work such as CmdFillMemory). Compute submits never present
// frames. sceGnmWaitQueueIdle blocks (guest-cooperative, exit-aware) until
// the selected queues drain: bit0 = graphics, bit1 = compute (3 = both).
#define PSB_QUEUE_GRAPHICS 0x1u
#define PSB_QUEUE_COMPUTE  0x2u
int32_t sceGnmSubmitCompute(uint32_t count, void** ccbAddrs,
                            uint32_t* ccbSizesInBytes);
int32_t sceGnmWaitQueueIdle(uint32_t queueMask);

// ---- Audio out (v1.3 — GUEST_ABI §9) ---------------------------------------------
// PSBET mixes every open port into one fixed 48 kHz stereo master stream
// (guest-side mixing semantics). Port handles are 1-based; -1 on failure.
#define PSB_AUDIO_FMT_S16_MONO     0u
#define PSB_AUDIO_FMT_S16_STEREO   1u
#define PSB_AUDIO_FMT_FLOAT_STEREO 2u
#define PSB_AUDIO_PORT_MAIN        0u
#define PSB_AUDIO_PORT_BGM         1u
#define PSB_AUDIO_PORT_VOICE       2u
#define PSB_AUDIO_VOL_MAX          32768
#define PSB_AUDIO_CH_LEFT          0x1u
#define PSB_AUDIO_CH_RIGHT         0x2u

int32_t sceAudioOutOpen(int32_t userId, uint32_t portType, uint32_t index,
                        uint32_t lenFrames, uint32_t freq, uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, void* ptr);       // submit lenFrames
int32_t sceAudioOutSetVolume(int32_t handle, uint32_t chBitmask,
                             int32_t* volumes);              // per-channel 0..MAX
int32_t sceAudioOutClose(int32_t handle);

// ---- Pad / input (v1.3 — GUEST_ABI §10) --------------------------------------------
#define PSB_PAD_BTN_CROSS     (1u << 0)
#define PSB_PAD_BTN_CIRCLE    (1u << 1)
#define PSB_PAD_BTN_SQUARE    (1u << 2)
#define PSB_PAD_BTN_TRIANGLE  (1u << 3)
#define PSB_PAD_BTN_L1        (1u << 4)
#define PSB_PAD_BTN_R1        (1u << 5)
#define PSB_PAD_BTN_L2        (1u << 6)
#define PSB_PAD_BTN_R2        (1u << 7)
#define PSB_PAD_BTN_OPTIONS   (1u << 8)
#define PSB_PAD_BTN_SHARE     (1u << 9)
#define PSB_PAD_BTN_PS        (1u << 10)
#define PSB_PAD_BTN_TOUCHPAD  (1u << 11)
#define PSB_PAD_BTN_UP        (1u << 12)
#define PSB_PAD_BTN_DOWN      (1u << 13)
#define PSB_PAD_BTN_LEFT      (1u << 14)
#define PSB_PAD_BTN_RIGHT     (1u << 15)
#define PSB_PAD_BTN_L3        (1u << 16)
#define PSB_PAD_BTN_R3        (1u << 17)

typedef struct PsbPadState {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;      // sticks 0..255 (center 128)
    uint8_t  l2, r2;              // analog triggers 0..255
    float    accelX, accelY, accelZ;   // g
    float    gyroX, gyroY, gyroZ;      // deg/s
    uint32_t connected;
    uint64_t timestampUs;         // guest time this state became valid
} PsbPadState;

typedef struct PsbPadControllerInfo {
    uint32_t touchpixW, touchpixH;   // touchpad pixel resolution
    uint32_t touchresX, touchresY;   // touchpad sampling resolution
    uint32_t connectionType;         // 0 none / 1 usb / 2 bluetooth
    uint32_t connectedCount;
} PsbPadControllerInfo;

int32_t scePadOpen(int32_t userId, int32_t type, int32_t index,
                   const void* param);
int32_t scePadReadState(int32_t handle, PsbPadState* out);
int32_t scePadRead(int32_t handle, PsbPadState* out, uint32_t count,
                   uint32_t* returned);
int32_t scePadGetControllerInformation(int32_t handle,
                                       PsbPadControllerInfo* out);

// ---- Network (v1.3 — GUEST_ABI §11: stubbed, logged, no connectivity) ------------
#define PSB_NET_ERR_NOT_INITIALIZED (-0x4E01)
#define PSB_NET_ERR_UNAVAILABLE     (-0x4E02)
#define PSB_NET_ERR_INVALID_ARG     (-0x4E03)

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
int32_t sceNetCtlGetState(int32_t* stateOut);   // always reports disconnected

#ifdef __cplusplus
}
#endif
