// ============================================================================
//  PSBET — kernel_shim.cpp
//  Guest-visible kernel surface implemented on host primitives.
//
//  Every function here is clean-room work: PS5-style semantics, original
//  implementation, no Sony code or constants.
// ============================================================================
#include "kernel_shim.h"

#if defined(_WIN32)
// Host->guest ABI bridge (w64_thunks.asm): MSVC host code launching a guest
// worker — declared early; sceKernelStartThread uses it below.
extern "C" int32_t psbTWinGuestThreadEntry(size_t arglen, void* argp,
                                           void* entry);
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <setjmp.h>
#include <thread>

#include "core/log.h"
#include "core/memory.h"
#include "core/vfs.h"
#include "core/syscall_trap.h"
#include "core/timing.h"
#include "core/audio.h"
#include "core/pad.h"
#include "core/net_stubs.h"
#include "psbet/psb_guest_api.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace psb::kernel {

// ---------------------------------------------------------------------------
// Cooperative wake state — every blocking kernel primitive and every parked
// thread waits on these, so exit/shutdown can always make progress.
// ---------------------------------------------------------------------------
namespace {
std::mutex              g_wakeMtx;
std::condition_variable g_wakeCv;
}

// ---------------------------------------------------------------------------
// Internal record types + thread-local state
// ---------------------------------------------------------------------------
namespace {

struct ThreadRec {
    std::string   name;
    void*         entry = nullptr;
    i32           priority = 0;
    u64           stackSize = 0;
    size_t        argLen = 0;
    std::vector<u8> argBuf;
    std::thread   thread;
    i32           retVal = 0;
    bool          started = false;
    bool          finished = false;
    bool          parked = false;   // Windows recorded-exit park (never unwound)
};

struct MutexRec { std::recursive_mutex mtx; };
struct SemaRec {
    std::mutex mtx;
    std::condition_variable cv;
    i64 count = 0;
    i64 max = 0;
};
struct CondRec { std::condition_variable_any cv; };

// EQUEUE (v1.2): a wakeable event queue. Producers push; WaitEqueue pops one
// pending event; every wait wakes on process exit / shutdown.
struct EqueueRec {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<PsbEqueueEvent> pending;
    std::string name;
};

thread_local std::map<u64, std::vector<u8>> g_tlsBlocks;
thread_local bool   g_inGuestThread = false;
thread_local bool   g_threadExitEnvValid = false;
thread_local i32    g_pendingRetVal = 0;
thread_local ThreadRec* g_selfThread = nullptr;  // this thread's kernel record

// Each guest thread gets its own exit env so sceKernelExitThread can unwind
// only that thread. Buffer is per-thread; used solely inside its own thread.
struct ThreadExitEnv { jmp_buf buf; };
thread_local ThreadExitEnv g_threadExitEnv;

jmp_buf* g_processEnv = nullptr;   // owned by CPU supervisor (main thread)
std::atomic<i32> g_exitInFlight{0};
std::atomic<bool> g_shutdown{false};

// Supervisor thread identity — set by runGuestEntry via beginSupervisorScope.
thread_local bool g_isSupervisor = false;

// Serializes all guest-visible console writes (guest sceKernelWrite + engine
// log tail) so lines never interleave mid-line (Phase-1 flaw fix).
std::mutex g_consoleMtx;

} // namespace

// ---------------------------------------------------------------------------
// KernelState
// ---------------------------------------------------------------------------
KernelState& KernelState::get() {
    static KernelState s;
    return s;
}

void KernelState::init(MemoryManager* mem) {
    std::lock_guard<std::mutex> lk(mtx_);
    mem_ = mem;
    handles_.clear();
    handleKinds_.clear();
    exitRequested_ = false;
    exitCode_ = 0;
    g_exitInFlight.store(0);   // allow a fresh exit path for this guest run
    g_shutdown.store(false);   // fresh cooperative-shutdown state
    g_wakeCv.notify_all();
    // Guest clock domain (Phase 4): fresh origin per run + teardown-aware
    // sleeps (exit/shutdown wake blocking guest timing primitives).
    time::TimeDomain::get().init();
    time::TimeDomain::get().setExitPoll(
        [] { return KernelState::get().exitRequested(); },
        [] { return shutdownRequested(); });
}

// --- cooperative wake state -------------------------------------------------

void requestShutdown() {
    g_shutdown.store(true);
    g_wakeCv.notify_all();
}

bool shutdownRequested() { return g_shutdown.load(); }

i32 runGuestMainCooperative(void* entry, bool* done) {
    g_inGuestThread = true;
    g_pendingRetVal = 0;
    g_selfThread = nullptr;   // the guest main is not a tracked ThreadRec
    installThreadTls();       // syscall trap + TLS on this guest thread only
    if (setjmp(g_threadExitEnv.buf) == 0) {
        g_threadExitEnvValid = true;
        PsbThreadEntry fn = reinterpret_cast<PsbThreadEntry>(entry);
        fn(0, nullptr);       // natural return or ExitThread unwind
    }
    g_threadExitEnvValid = false;
    g_inGuestThread = false;
    if (done) *done = true;
    // The caller (supervisor) uses the recorded process exit code whenever
    // an exit was requested; a natural return with no exit is code 0.
    KernelState& k = KernelState::get();
    return k.exitRequested() ? k.exitCode() : g_pendingRetVal;
}

bool isSupervisorThread() { return g_isSupervisor; }

void beginSupervisorScope() { g_isSupervisor = true; }
void endSupervisorScope()   { g_isSupervisor = false; }

// Wait helper: waits on `cv` with predicate `pred`, but wakes when either the
// predicate is satisfied, the process exit was requested, or shutdown began.
// Returns false when woken by exit/shutdown (caller returns an error).
template <typename Cv, typename Pred>
static bool wakeableWait(std::unique_lock<std::mutex>& lk, Cv& cv, Pred pred) {
    if (pred()) return true;
    while (!pred()) {
        if (KernelState::get().exitRequested() || shutdownRequested())
            return pred(); // may be false — caller decides
        cv.wait_for(lk, std::chrono::milliseconds(2), pred);
    }
    return true;
}

template <typename Pred>
static bool wakeableWaitAny(std::unique_lock<std::recursive_mutex>& lk,
                            std::condition_variable_any& cv, Pred pred) {
    if (pred()) return true;
    while (!pred()) {
        if (KernelState::get().exitRequested() || shutdownRequested())
            return pred();
        cv.wait_for(lk, std::chrono::milliseconds(2), pred);
    }
    return true;
}

i32 KernelState::addHandle(std::shared_ptr<void> obj, const char* kind) {
    std::lock_guard<std::mutex> lk(mtx_);
    handles_.push_back(std::move(obj));
    handleKinds_.emplace_back(kind ? kind : "generic");
    return static_cast<i32>(handles_.size()); // 1-based; 0 reserved for failure
}

std::shared_ptr<void> KernelState::getHandle(i32 id, const char* kind) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (id <= 0 || static_cast<size_t>(id) > handles_.size()) return nullptr;
    if (kind && handleKinds_[static_cast<size_t>(id) - 1] != kind) return nullptr;
    return handles_[static_cast<size_t>(id) - 1];
}

void KernelState::registerTlsModule(u32 id, const void* image, u64 memSize) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (id != 1) {
        LOG_WARN("TLS: multi-module TLS not supported in Phase 1 (module %u ignored)", id);
        return;
    }
    tlsModule_.image.assign(static_cast<const u8*>(image),
                            static_cast<const u8*>(image) + memSize);
    tlsModule_.memSize = memSize;
    tlsModuleValid_ = true;
}

void* KernelState::tlsGetAddr(PsbTlsIndex* idx) {
    if (!idx || idx->module != 1) return nullptr;
    auto& block = g_tlsBlocks[idx->module];
    if (block.empty()) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!tlsModuleValid_) {
            LOG_WARN("TLS: guest uses TLS but no module registered it");
            block.assign(64, 0);
        } else {
            block = tlsModule_.image;
            block.resize(static_cast<size_t>(tlsModule_.memSize));
        }
    }
    return block.data() + idx->offset;
}

void installThreadTls() {
    // Raw-syscall containment: every thread that executes guest code gets
    // the seccomp trap (Linux) — main supervisor thread and workers alike.
    syscall_trap::installOnCurrentThread(KernelState::get().syscallTrapMode());

    PsbTlsIndex idx{1, 0};
    (void)KernelState::get().tlsGetAddr(&idx); // force per-thread block creation
}

void setProcessJmpBuf(void* env) { g_processEnv = static_cast<jmp_buf*>(env); }
void* processJmpBuf()            { return g_processEnv; }

void KernelState::requestProcessExit(i32 exitCode) {
    i32 expected = 0;
    const bool first = g_exitInFlight.compare_exchange_strong(expected, 1);
    if (first) {
        exitCode_ = exitCode;
        exitRequested_ = true;
        g_wakeCv.notify_all(); // wake anyone blocked in kernel primitives
    }

#if defined(_WIN32)
    // Windows hosting model: no longjmp anywhere (MSVC SEH dispatch cannot
    // unwind GCC-compiled guest frames). Exit is a recorded, signaled state;
    // the supervisor's wait returns, and the calling guest thread parks in
    // host code below — detached at shutdown (the process is exiting).
    g_wakeCv.notify_all();
#else
    if (isSupervisorThread()) {
        // Same-thread exit: unwind guest frames through the supervisor env.
        if (g_processEnv) longjmp(*g_processEnv, 1);
        LOG_ERROR("Kernel: process exit without supervisor env — forcing exit(%d)", exitCode);
        std::quick_exit(exitCode);
    }
#endif

#if !defined(_WIN32)
    // POSIX worker thread: unwind OUR OWN guest frames via our thread exit
    // env — the exact mechanism as sceKernelExitThread. The thread then
    // finishes normally and stays joinable (Phase-1 parked-forever flaw
    // fixed).
    if (g_threadExitEnvValid) {
        g_pendingRetVal = first ? exitCode : 0;
        longjmp(g_threadExitEnv.buf, 1);
    }
#endif

    // Worker outside guest frames, or any worker on Windows: park until
    // shutdown, then block indefinitely in host code — never unwinding
    // guest frames.
    LOG_WARN("Kernel: ExitProcess from non-supervisor thread — parking until shutdown");
    std::unique_lock<std::mutex> lk(g_wakeMtx);
    g_wakeCv.wait(lk, [] { return shutdownRequested(); });
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

bool KernelState::exitRequested() const { return exitRequested_; }
i32  KernelState::exitCode() const      { return exitCode_; }

void KernelState::shutdownThreads() {
    requestShutdown(); // wake every primitive blocked in this process
    ThreadStats stats;

    // Join every finished thread; give running ones a bounded cooperative
    // grace (they were woken by requestShutdown).
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(200);
    bool allDone = false;
    while (!allDone && std::chrono::steady_clock::now() <= deadline) {
        allDone = true;
        std::vector<std::shared_ptr<ThreadRec>> batch;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (size_t i = 0; i < handles_.size(); ++i) {
                if (handleKinds_[i] != "thread") continue;
                auto rec = std::static_pointer_cast<ThreadRec>(handles_[i]);
                if (rec->thread.joinable() && rec->finished && !rec->parked)
                    batch.push_back(rec);
                if (rec->thread.joinable() && !rec->finished && !rec->parked)
                    allDone = false;
            }
        }
        for (auto& rec : batch) {
            if (rec->thread.joinable()) { rec->thread.join(); ++stats.joined; }
        }
        if (!allDone) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Phase-3 acceptance: zero force-detached threads on the POSIX path.
    // Anything still running after the grace period is a real straggler —
    // detach (loudly) and account it. Windows recorded-exit parks are never
    // counted as stragglers: they are joinable-dead threads detached quietly
    // with their own accounting line.
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < handles_.size(); ++i) {
        if (handleKinds_[i] != "thread") continue;
        auto rec = std::static_pointer_cast<ThreadRec>(handles_[i]);
        if (!rec->thread.joinable()) continue;
        if (rec->parked) {
            LOG_DEBUG("kernel: parked guest thread '%s' detached (recorded exit)",
                      rec->name.c_str());
            rec->thread.detach();
            ++stats.parked;
        } else if (!rec->finished) {
            LOG_WARN("kernel: guest thread '%s' still running at shutdown — "
                     "force-detaching (teardown leak)", rec->name.c_str());
            rec->thread.detach();
            ++stats.detached;
        }
    }
    threadStats_ = stats;
}

kernel::ThreadStats KernelState::lastThreadStats() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return threadStats_;
}

void KernelState::setSyscallTrapMode(syscall_trap::Mode m) {
    std::lock_guard<std::mutex> lk(mtx_);
    trapMode_ = m;
}

syscall_trap::Mode KernelState::syscallTrapMode() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return trapMode_;
}

void KernelState::setProcessStart(u64) {
    // Phase 4: the guest clock lives in the TimeDomain (fresh origin at
    // KernelState::init). The emulator's host-side anchor is engine-internal
    // bookkeeping and no longer feeds guest-visible time.
}

u64 KernelState::processTimeUs() const {
    // Monotonic + drift-free: derived from one origin sample, no accumulation.
    return time::TimeDomain::get().us();
}

// ---------------------------------------------------------------------------
// Guest-visible functions (extern "C")
// ---------------------------------------------------------------------------
extern "C" {

// ---- memory ---------------------------------------------------------------
// Phase-3 faithful model: typed pools honour search ranges, map is tracked by
// page tables, free is refcount-guarded, and the GPU owns its own VA space.
// See guest_vm.h for the model and GUEST_ABI §5 for guest-visible semantics.
static vm::Prot protFromBits(int prot) {
    vm::Prot p = vm::Prot::NoAccess;
    if (prot & PSB_PROT_READ)  p = static_cast<vm::Prot>(static_cast<u32>(p) | static_cast<u32>(vm::Prot::ReadOnly));
    if (prot & PSB_PROT_WRITE) p = static_cast<vm::Prot>(static_cast<u32>(p) | static_cast<u32>(vm::Prot::Write));
    if (prot & PSB_PROT_EXEC)  p = static_cast<vm::Prot>(static_cast<u32>(p) | static_cast<u32>(vm::Prot::Exec));
    return p;
}

int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd,
                                      size_t len, size_t alignment,
                                      int memoryType, int64_t* physAddrOut) {
    KernelState& k = KernelState::get();
    if (!physAddrOut || len == 0) return -1;
    u32 pool = GuestMemory::kPoolRegular;
    if (memoryType == 1) pool = GuestMemory::kPoolGpuVisible;
    else if (memoryType != 0)
        LOG_WARN("kernel: memoryType %d mapped to the regular pool", memoryType);
    const u64 pa = k.memory()->guest().allocDirect(
        pool, len, alignment ? alignment : 1,
        static_cast<u64>(searchStart), static_cast<u64>(searchEnd),
        "guest-direct");
    if (!pa) return -1;
    *physAddrOut = static_cast<int64_t>(pa);
    return 0;
}

int32_t sceKernelFreeDirectMemory(int64_t physAddr, size_t len) {
    KernelState& k = KernelState::get();
    if (physAddr <= 0 || len == 0) return -1;
    return k.memory()->guest().freeDirect(static_cast<u64>(physAddr), len) ? 0 : -1;
}

static int32_t mapDirectImpl(void** addrOut, size_t len, int prot, int flags,
                             int64_t physAddr, const char* name) {
    KernelState& k = KernelState::get();
    if (!addrOut || len == 0 || physAddr <= 0) return -1;
    const u64 pa = static_cast<u64>(physAddr);
    const u64 va = k.memory()->guest().mapDirect(pa, len, protFromBits(prot), name);
    if (!va) return -1;
    if (flags & PSB_MAP_FIXED) {
        // Identity model: a fixed CPU VA must be exactly arena+PA.
        const u64 want = *reinterpret_cast<u64*>(addrOut);
        if (want != va) {
            LOG_ERROR("kernel: fixed CPU map refused — guest asked 0x%llx, "
                      "identity mapping is 0x%llx (CPU VA!=PA remapping is "
                      "not supported in v1.2; see GUEST_ABI §5)",
                      static_cast<unsigned long long>(want),
                      static_cast<unsigned long long>(va));
            k.memory()->guest().unmap(va, len);
            return -1;
        }
    }
    *addrOut = reinterpret_cast<void*>(va);
    return 0;
}

int32_t sceKernelMapDirectMemory(void** addrOut, size_t len, int prot, int flags,
                                 int64_t physAddr, size_t pageSizeArg) {
    (void)pageSizeArg;
    return mapDirectImpl(addrOut, len, prot, flags, physAddr, "guest-map");
}

int32_t sceKernelMapNamedDirectMemory(void** addrOut, size_t len, int prot,
                                      int flags, int64_t physAddr,
                                      size_t pageSize, const char* name) {
    (void)pageSize;
    return mapDirectImpl(addrOut, len, prot, flags, physAddr,
                         name ? name : "guest-named-map");
}

int32_t sceKernelGetDirectMemorySize(size_t* out) {
    if (!out) return -1;
    *out = static_cast<size_t>(MemoryManager::kArenaSize);
    return 0;
}

int32_t sceKernelMmap(void** addrOut, size_t len, int prot, int flags,
                      int32_t fd, int64_t offset) {
    if (fd >= 0 || offset != 0) {
        LOG_WARN("kernel: Mmap of fd-backed memory is not supported in v1.2 "
                 "(anonymous only)");
        return -1;
    }
    (void)flags;
    KernelState& k = KernelState::get();
    if (!addrOut || len == 0) return -1;
    const u64 pa = k.memory()->guest().allocDirect(
        GuestMemory::kPoolRegular, len, 0x1000, 0, 0, "guest-anon");
    if (!pa) return -1;
    const u64 va = k.memory()->guest().mapDirect(pa, len, protFromBits(prot),
                                                 "guest-anon");
    if (!va) {
        k.memory()->guest().freeDirect(pa, len);
        return -1;
    }
    *addrOut = reinterpret_cast<void*>(va);
    return 0;
}

int32_t sceKernelMunmap(void* addr, size_t len) {
    if (!addr || len == 0) return -1;
    KernelState& k = KernelState::get();
    const u64 va = reinterpret_cast<u64>(addr);
    GuestMemory::BlockInfo info;
    if (!k.memory()->guest().queryVa(va, &info) || info.len != len)
        return -1;
    if (!k.memory()->guest().unmap(va, len)) return -1;
    return k.memory()->guest().freeDirect(info.pa, len) ? 0 : -1;
}

int32_t sceKernelQueryMemoryProtection(void* addr, void** startOut,
                                       void** endOut, int32_t* protOut) {
    if (!addr || !startOut || !endOut || !protOut) return -1;
    KernelState& k = KernelState::get();
    GuestMemory::BlockInfo info;
    if (!k.memory()->guest().queryVa(reinterpret_cast<u64>(addr), &info))
        return -1;
    *startOut = reinterpret_cast<void*>(info.va);
    *endOut = reinterpret_cast<void*>(info.va + info.len);
    int p = 0;
    if (static_cast<u32>(info.prot) & static_cast<u32>(vm::Prot::ReadOnly)) p |= PSB_PROT_READ;
    if (static_cast<u32>(info.prot) & static_cast<u32>(vm::Prot::Write))    p |= PSB_PROT_WRITE;
    if (static_cast<u32>(info.prot) & static_cast<u32>(vm::Prot::Exec))     p |= PSB_PROT_EXEC;
    *protOut = p;
    return 0;
}

// ---- time -----------------------------------------------------------------
uint64_t sceKernelGetProcessTime(void) {
    return KernelState::get().processTimeUs();
}

uint64_t sceKernelGetProcessTimeInTick(void) {
    // PSBET tick = 1 ns (documented guest ABI v1).
    return sceKernelGetProcessTime() * 1000ull;
}

uint64_t sceKernelGetProcessTimeCounter(void) {
    // v1.3: counter domain == ns domain (frequency is a fixed 1 GHz).
    return time::TimeDomain::get().ns();
}

uint64_t sceKernelGetProcessTimeCounterFrequency(void) {
    return time::TimeDomain::kCounterFrequency;
}

int32_t sceKernelUsleep(uint64_t microseconds) {
    // Cooperative + exit-aware (Phase 4): a sleeping guest wakes when the
    // process exit is requested or shutdown begins — sleeps can no longer
    // stall teardown. Return value 0 in every case (documented).
    time::TimeDomain::get().sleepUs(microseconds);
    return 0;
}

int32_t sceKernelNanosleep(uint64_t seconds, uint64_t nanoseconds) {
    // Overflow-safe total (huge requests clamp instead of wrapping).
    const u64 usFromSec = seconds > (~0ull / 1'000'000ull)
                              ? ~0ull : seconds * 1'000'000ull;
    const u64 total = usFromSec > (~0ull - nanoseconds / 1000ull)
                          ? ~0ull : usFromSec + nanoseconds / 1000ull;
    time::TimeDomain::get().sleepUs(total);
    return 0;
}

int32_t sceKernelSleep(uint64_t seconds) {
    return sceKernelNanosleep(seconds, 0);
}

// ---- threads --------------------------------------------------------------
int32_t sceKernelCreateThread(const char* name, void* entry, int32_t initPriority,
                              size_t stackSize, uint64_t attr,
                              uint64_t cpuAffinityMask, int32_t* out) {
    (void)attr; (void)cpuAffinityMask; // Phase 1: accepted, unused
    if (!name || !entry || !out) return -1;

    auto rec = std::make_shared<ThreadRec>();
    rec->name = name;
    rec->entry = entry;
    rec->priority = initPriority;
    rec->stackSize = stackSize ? stackSize : (1u << 20);

    *out = KernelState::get().addHandle(rec, "thread");
    LOG_DEBUG("kernel: thread '%s' created (tid=%d, stack=%zu KiB)",
              name, *out, rec->stackSize / 1024);
    return 0;
}

int32_t sceKernelStartThread(int32_t tid, size_t arglen, void* argp) {
    auto rec = std::static_pointer_cast<ThreadRec>(
        KernelState::get().getHandle(tid, "thread"));
    if (!rec || rec->started) return -1;

    rec->argLen = arglen;
    if (arglen && argp)
        rec->argBuf.assign(static_cast<u8*>(argp), static_cast<u8*>(argp) + arglen);
    rec->started = true;

    ThreadRec* raw = rec.get();
    raw->thread = std::thread([raw]() {
        g_inGuestThread = true;
        g_pendingRetVal = 0;
        g_selfThread = raw;
        installThreadTls();
        if (setjmp(g_threadExitEnv.buf) == 0) {
            g_threadExitEnvValid = true;
#if defined(_WIN32)
            // Host->guest ABI bridge: the MSVC-compiled lambda cannot call
            // the GCC-compiled guest entry directly (Win64 RCX/RDX vs SysV
            // RDI/RSI) — the bridge shuffles; the guest's int32 return
            // arrives in EAX, the return register of both ABIs.
            g_pendingRetVal = psbTWinGuestThreadEntry(
                raw->argBuf.size(),
                raw->argBuf.empty()
                    ? nullptr
                    : static_cast<void*>(raw->argBuf.data()),
                raw->entry);
#else
            PsbThreadEntry fn = reinterpret_cast<PsbThreadEntry>(raw->entry);
            g_pendingRetVal = fn(raw->argBuf.size(),
                                 raw->argBuf.empty()
                                     ? nullptr
                                     : static_cast<void*>(raw->argBuf.data()));
#endif
        }
        // Reached via sceKernelExitThread longjmp or natural return.
        g_threadExitEnvValid = false;
        g_selfThread = nullptr;
        raw->retVal = g_pendingRetVal;
        raw->finished = true;
    });
    return 0;
}

int32_t sceKernelWaitThreadEnd(int32_t tid, int32_t* retVal, uint64_t* timeoutUs) {
    (void)timeoutUs; // cooperative unbounded wait
    auto rec = std::static_pointer_cast<ThreadRec>(
        KernelState::get().getHandle(tid, "thread"));
    if (!rec) return -1;
    if (rec->thread.joinable()) {
        // Poll the finished flag so a worker-initiated process exit wakes us.
        while (!rec->finished) {
            if (KernelState::get().exitRequested() || shutdownRequested()) {
                LOG_DEBUG("kernel: WaitThreadEnd aborted by exit/shutdown");
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        // Windows recorded-exit parks leave the OS thread alive on purpose
        // (MSVC SEH cannot unwind GCC guest frames). They are detached at
        // teardown; joining them here would deadlock forever.
        if (!rec->parked) rec->thread.join();
    }
    if (retVal) *retVal = rec->retVal;
    return 0;
}

void sceKernelExitThread(int32_t retVal) {
    if (!g_inGuestThread) {
        LOG_WARN("kernel: ExitThread outside a guest thread — ignoring");
        return;
    }
#if defined(_WIN32)
    // Windows hosting model: a longjmp from a GCC-compiled guest frame
    // triggers STATUS_INVALID_DISPATCH under MSVC SEH (Phase-2 discovery).
    // Record the result, mark the thread finished (WaitThreadEnd returns
    // immediately without joining) and park — never unwinding guest frames.
    if (g_selfThread) {
        g_selfThread->retVal = retVal;
        g_selfThread->finished = true;
        g_selfThread->parked = true;
        g_selfThread = nullptr;
    }
    LOG_DEBUG("kernel: ExitThread(%d) — Windows record+park", retVal);
    std::unique_lock<std::mutex> lk(g_wakeMtx);
    g_wakeCv.wait(lk, [] { return shutdownRequested(); });
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
#else
    if (!g_threadExitEnvValid) {
        LOG_WARN("kernel: ExitThread outside guest frames — ignoring");
        return;
    }
    g_pendingRetVal = retVal;
    longjmp(g_threadExitEnv.buf, 1);
#endif
}

int32_t sceKernelGetThreadId(void) {
    const auto h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<int32_t>(h & 0x7fffffffu);
}

void sceKernelExitProcess(int32_t status) {
    KernelState::get().requestProcessExit(status);
}

// ---- mutexes --------------------------------------------------------------
int32_t sceKernelCreateMutex(const char* name, uint32_t attr, int32_t initLocked,
                             int32_t* out) {
    (void)attr; // Phase 1: all mutexes are recursive-safe
    if (!name || !out) return -1;
    auto rec = std::make_shared<MutexRec>();
    if (initLocked) rec->mtx.lock();
    *out = KernelState::get().addHandle(rec, "mutex");
    LOG_DEBUG("kernel: mutex '%s' created (id=%d)", name, *out);
    return 0;
}

int32_t sceKernelLockMutex(int32_t mutexId, uint64_t timeoutUs) {
    (void)timeoutUs; // Phase 1: unbounded waits
    auto rec = std::static_pointer_cast<MutexRec>(
        KernelState::get().getHandle(mutexId, "mutex"));
    if (!rec) return -1;
    rec->mtx.lock();
    return 0;
}

int32_t sceKernelUnlockMutex(int32_t mutexId) {
    auto rec = std::static_pointer_cast<MutexRec>(
        KernelState::get().getHandle(mutexId, "mutex"));
    if (!rec) return -1;
    rec->mtx.unlock();
    return 0;
}

// ---- semaphores -----------------------------------------------------------
int32_t sceKernelCreateSemaphore(const char* name, int32_t initCount,
                                 int32_t maxCount, int32_t* out) {
    if (!name || !out || maxCount <= 0 || initCount < 0 || initCount > maxCount)
        return -1;
    auto rec = std::make_shared<SemaRec>();
    rec->count = initCount;
    rec->max = maxCount;
    *out = KernelState::get().addHandle(rec, "sema");
    LOG_DEBUG("kernel: semaphore '%s' created (id=%d)", name, *out);
    return 0;
}

int32_t sceKernelWaitSemaphore(int32_t semaId, int32_t needCount, uint64_t timeoutUs) {
    (void)timeoutUs; // Phase 2: unbounded waits are cooperative (see below)
    auto rec = std::static_pointer_cast<SemaRec>(
        KernelState::get().getHandle(semaId, "sema"));
    if (!rec || needCount <= 0) return -1;
    std::unique_lock<std::mutex> lk(rec->mtx);
    if (!wakeableWait(lk, rec->cv, [&] { return rec->count >= needCount; })) {
        LOG_DEBUG("kernel: WaitSemaphore aborted by exit/shutdown");
        return -1;
    }
    rec->count -= needCount;
    return 0;
}

int32_t sceKernelSignalSemaphore(int32_t semaId, int32_t signalCount) {
    auto rec = std::static_pointer_cast<SemaRec>(
        KernelState::get().getHandle(semaId, "sema"));
    if (!rec || signalCount <= 0) return -1;
    {
        std::lock_guard<std::mutex> lk(rec->mtx);
        rec->count = std::min<i64>(rec->count + signalCount, rec->max);
    }
    rec->cv.notify_all();
    return 0;
}

// ---- condition variables --------------------------------------------------
int32_t sceKernelCreateCond(const char* name, uint32_t attr, int32_t* out) {
    (void)attr;
    if (!name || !out) return -1;
    *out = KernelState::get().addHandle(std::make_shared<CondRec>(), "cond");
    LOG_DEBUG("kernel: cond '%s' created (id=%d)", name, *out);
    return 0;
}

int32_t sceKernelWaitCond(int32_t condId, int32_t mutexId, uint64_t timeoutUs) {
    auto cond = std::static_pointer_cast<CondRec>(
        KernelState::get().getHandle(condId, "cond"));
    auto mtx = std::static_pointer_cast<MutexRec>(
        KernelState::get().getHandle(mutexId, "mutex"));
    if (!cond || !mtx) return -1;
    std::unique_lock<std::recursive_mutex> lk(mtx->mtx);
    if (timeoutUs == 0) {
        wakeableWaitAny(lk, cond->cv, [] { return false; }); // pure wait: wake only
    } else {
        cond->cv.wait_for(lk, std::chrono::microseconds(timeoutUs));
    }
    return 0;
}

int32_t sceKernelSignalCond(int32_t condId) {
    auto cond = std::static_pointer_cast<CondRec>(
        KernelState::get().getHandle(condId, "cond"));
    if (!cond) return -1;
    cond->cv.notify_one();
    return 0;
}

int32_t sceKernelSignalCondAll(int32_t condId) {
    auto cond = std::static_pointer_cast<CondRec>(
        KernelState::get().getHandle(condId, "cond"));
    if (!cond) return -1;
    cond->cv.notify_all();
    return 0;
}

// ---- event queues (EQUEUE v1.2) --------------------------------------------
int32_t sceKernelCreateEqueue(const char* name, int32_t* out) {
    if (!name || !out) return -1;
    auto rec = std::make_shared<EqueueRec>();
    rec->name = name;
    *out = KernelState::get().addHandle(rec, "equeue");
    LOG_DEBUG("kernel: equeue '%s' created (id=%d)", name, *out);
    return 0;
}

int32_t sceKernelDeleteEqueue(int32_t eqId) {
    auto rec = std::static_pointer_cast<EqueueRec>(
        KernelState::get().getHandle(eqId, "equeue"));
    if (!rec) return -1;
    {
        std::lock_guard<std::mutex> lk(rec->mtx);
        rec->pending.clear();
    }
    rec->cv.notify_all();
    return 0;
}

int32_t sceKernelAddEvent(int32_t eqId, uint64_t ident, uint32_t flags,
                          uint64_t userData) {
    auto rec = std::static_pointer_cast<EqueueRec>(
        KernelState::get().getHandle(eqId, "equeue"));
    if (!rec) return -1;
    {
        std::lock_guard<std::mutex> lk(rec->mtx);
        rec->pending.push_back({ident, flags, userData});
    }
    rec->cv.notify_all();
    return 0;
}

int32_t sceKernelTriggerEvent(int32_t eqId, uint64_t ident, uint32_t flags,
                              uint64_t userData) {
    // v1.2 semantics: TriggerEvent is the kernel-side signal path — identical
    // observable effect to AddEvent (documented in GUEST_ABI §7).
    return sceKernelAddEvent(eqId, ident, flags, userData);
}

int32_t sceKernelWaitEqueue(int32_t eqId, PsbEqueueEvent* eventOut,
                            uint64_t timeoutUs) {
    auto rec = std::static_pointer_cast<EqueueRec>(
        KernelState::get().getHandle(eqId, "equeue"));
    if (!rec || !eventOut) return -1;
    std::unique_lock<std::mutex> lk(rec->mtx);
    const auto pred = [&] { return !rec->pending.empty(); };
    bool got = false;
    if (timeoutUs == 0) {
        got = wakeableWait(lk, rec->cv, pred);
    } else {
        const auto until = std::chrono::steady_clock::now()
                         + std::chrono::microseconds(timeoutUs);
        while (!pred()) {
            if (KernelState::get().exitRequested() || shutdownRequested()) break;
            if (rec->cv.wait_until(lk, until) == std::cv_status::timeout) break;
        }
        got = pred();
    }
    if (!got) return -1; // timeout or exit/shutdown wake
    *eventOut = rec->pending.front();
    rec->pending.erase(rec->pending.begin());
    return 0;
}

int32_t sceKernelCancelEvent(int32_t eqId, uint64_t ident) {
    auto rec = std::static_pointer_cast<EqueueRec>(
        KernelState::get().getHandle(eqId, "equeue"));
    if (!rec) return -1;
    std::lock_guard<std::mutex> lk(rec->mtx);
    auto& v = rec->pending;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const PsbEqueueEvent& e) { return e.ident == ident; }),
            v.end());
    return 0;
}

// ---- I/O ------------------------------------------------------------------
// Phase-3: the guest filesystem is virtualized (vfs.h). Guests never touch
// the host filesystem except through the /data passthrough mount. Console
// writes (fds 0/1/2) keep the serialized, flushed path.
int64_t sceKernelWrite(int32_t fd, const void* buf, size_t n) {
    if (!buf) return -1;
    if (fd == 0 || fd == 1 || fd == 2) {
        std::lock_guard<std::mutex> lk(g_consoleMtx);
#if !defined(_WIN32)
        const ssize_t r = ::write(fd, buf, n);
        if (fd == 1 || fd == 2) ::fsync(fd); // flush ordering vs host logging
        return static_cast<int64_t>(r);
#else
        HANDLE h = fd == 1 ? GetStdHandle(STD_OUTPUT_HANDLE)
                           : fd == 2 ? GetStdHandle(STD_ERROR_HANDLE) : nullptr;
        if (!h || h == INVALID_HANDLE_VALUE) return -1;
        DWORD written = 0;
        if (!WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr)) return -1;
        return static_cast<int64_t>(written);
#endif
    }
    return vfs::write(fd, buf, n);
}

int64_t sceKernelRead(int32_t fd, void* buf, size_t n) {
    if (!buf) return -1;
    if (fd == 0) {
#if !defined(_WIN32)
        return ::read(fd, buf, n);
#else
        return -1; // stdin reads arrive with the SDL input phase
#endif
    }
    if (fd == 1 || fd == 2) return -1;
    return vfs::read(fd, buf, n);
}

int32_t sceKernelOpen(const char* path, int32_t flags, uint32_t mode) {
    (void)mode; // POSIX 0777 semantics; the VFS layer decides what applies
    if (!path) return -1;
    return vfs::open(path, static_cast<u32>(flags));
}

int32_t sceKernelClose(int32_t fd) {
    if (fd >= 0 && fd <= 2) return 0; // std streams live for the process
    return vfs::close(fd);
}

int64_t sceKernelLseek(int32_t fd, int64_t offset, int32_t whence) {
    if (fd >= 0 && fd <= 2) return -1;
    return vfs::lseek(fd, offset, whence);
}

int32_t sceKernelFstat(int32_t fd, PsbStat* out) {
    if (!out || (fd >= 0 && fd <= 2)) return -1;
    vfs::FileStat st;
    if (!vfs::stat(fd, &st)) return -1;
    out->size = st.size;
    out->isDir = st.isDir ? 1u : 0u;
    out->readOnly = st.readOnly ? 1u : 0u;
    return 0;
}

// ---- misc -----------------------------------------------------------------
int32_t sceKernelIsNeoMode(void) {
    return 0; // base model
}

// ---- ABI conformance probes (Phase 3) --------------------------------------
// Deterministic argument-position checks used by the Windows SysV->Win64
// conformance suite (tests/homebrew/abi_probe.c). Each argument position is
// folded into the result with a distinct XOR constant and rotation, so any
// mis-routed register/stack slot is detectable. Present on every platform;
// on POSIX they pass trivially, on Windows they execute through the MASM
// thunks and would have caught the thunk3 arg2-loss bug.
namespace {
inline u64 psbRotl64(u64 x, unsigned r) { return (x << r) | (x >> (64 - r)); }
}

extern "C" {

uint64_t psbProbeCall1(uint64_t a) {
    return a ^ 0xA5u;
}

uint64_t psbProbeCall2(uint64_t a, uint64_t b) {
    return psbRotl64(a ^ 0xA5u, 7) ^ (b ^ 0x5Au);
}

uint64_t psbProbeCall3(uint64_t a, uint64_t b, uint64_t c) {
    return psbRotl64(psbRotl64(a ^ 0xA5u, 7) ^ (b ^ 0x5Au), 9) ^ (c ^ 0x33u);
}

uint64_t psbProbeCall4(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return psbRotl64(psbRotl64(psbRotl64(a ^ 0xA5u, 7) ^ (b ^ 0x5Au), 9)
                     ^ (c ^ 0x33u), 11) ^ (d ^ 0x99u);
}

uint64_t psbProbeCall5(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                       uint64_t e) {
    return psbRotl64(psbRotl64(psbRotl64(psbRotl64(a ^ 0xA5u, 7) ^ (b ^ 0x5Au), 9)
                               ^ (c ^ 0x33u), 11) ^ (d ^ 0x99u), 13)
         ^ (e ^ 0xC3u);
}

uint64_t psbProbeCall6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                       uint64_t e, uint64_t f) {
    return psbRotl64(psbRotl64(psbRotl64(psbRotl64(psbRotl64(a ^ 0xA5u, 7)
                     ^ (b ^ 0x5Au), 9) ^ (c ^ 0x33u), 11) ^ (d ^ 0x99u), 13)
                     ^ (e ^ 0xC3u), 17) ^ (f ^ 0x3Cu);
}

int32_t psbProbePtr2(void* out, uint64_t val) {
    if (!out) return -1;
    *reinterpret_cast<uint64_t*>(out) = val ^ 0x77u;
    return 0x1234;
}

} // extern "C" (ABI probes)

// ---- Windows ABI trampoline targets ---------------------------------------
// Guests call these through SysV->Win64 thunks (w64_thunks.asm): the thunk
// shuffles SysV registers (RDI,RSI,RDX,RCX...) into Win64 order (RCX,RDX,R8,
// R9) and reserves shadow space. Forwarders only — all logic stays here.
// (psbTWinGuestThreadEntry — the REVERSE bridge, host->guest worker launch —
// is declared at the top of this file: sceKernelStartThread uses it.)
#if defined(_WIN32)
// Defined later in this file (PSBET runtime section).
void*    psbTlsGetAddr(PsbTlsIndex* idx);
void     psbCxaFinalize(void* d);
int32_t  psbGetRuntimeVersion(void);
const char* psbGetRuntimeName(void);
void     psbStackChkFail(void);

extern "C" {
int32_t  psbXSysAllocateDirectMemory(int64_t a, int64_t b, size_t c, size_t d, int e, int64_t* f)
    { return sceKernelAllocateDirectMemory(a, b, c, d, e, f); }
int32_t  psbXSysMapDirectMemory(void** a, size_t b, int c, int d, int64_t e, size_t f)
    { return sceKernelMapDirectMemory(a, b, c, d, e, f); }
int32_t  psbXSysGetDirectMemorySize(size_t* a)
    { return sceKernelGetDirectMemorySize(a); }
uint64_t psbXSysGetProcessTime(void)
    { return sceKernelGetProcessTime(); }
uint64_t psbXSysGetProcessTimeInTick(void)
    { return sceKernelGetProcessTimeInTick(); }
uint64_t psbXSysGetProcessTimeCounter(void)
    { return sceKernelGetProcessTimeCounter(); }
uint64_t psbXSysGetProcessTimeCounterFrequency(void)
    { return sceKernelGetProcessTimeCounterFrequency(); }
int32_t  psbXSysUsleep(uint64_t a)
    { return sceKernelUsleep(a); }
int32_t  psbXSysNanosleep(uint64_t a, uint64_t b)
    { return sceKernelNanosleep(a, b); }
int32_t  psbXSysSleep(uint64_t a)
    { return sceKernelSleep(a); }
int32_t  psbXSysCreateThread(const char* a, void* b, int32_t c, size_t d, uint64_t e, uint64_t f, int32_t* g)
    { return sceKernelCreateThread(a, b, c, d, e, f, g); }
int32_t  psbXSysStartThread(int32_t a, size_t b, void* c)
    { return sceKernelStartThread(a, b, c); }
int32_t  psbXSysWaitThreadEnd(int32_t a, int32_t* b, uint64_t* c)
    { return sceKernelWaitThreadEnd(a, b, c); }
void     psbXSysExitThread(int32_t a)
    { sceKernelExitThread(a); }
void     psbXSysExitProcess(int32_t a)
    { sceKernelExitProcess(a); }
int32_t  psbXSysGetThreadId(void)
    { return sceKernelGetThreadId(); }
int32_t  psbXSysCreateMutex(const char* a, uint32_t b, int32_t c, int32_t* d)
    { return sceKernelCreateMutex(a, b, c, d); }
int32_t  psbXSysLockMutex(int32_t a, uint64_t b)
    { return sceKernelLockMutex(a, b); }
int32_t  psbXSysUnlockMutex(int32_t a)
    { return sceKernelUnlockMutex(a); }
int32_t  psbXSysCreateSemaphore(const char* a, int32_t b, int32_t c, int32_t* d)
    { return sceKernelCreateSemaphore(a, b, c, d); }
int32_t  psbXSysWaitSemaphore(int32_t a, int32_t b, uint64_t c)
    { return sceKernelWaitSemaphore(a, b, c); }
int32_t  psbXSysSignalSemaphore(int32_t a, int32_t b)
    { return sceKernelSignalSemaphore(a, b); }
int32_t  psbXSysCreateCond(const char* a, uint32_t b, int32_t* c)
    { return sceKernelCreateCond(a, b, c); }
int32_t  psbXSysWaitCond(int32_t a, int32_t b, uint64_t c)
    { return sceKernelWaitCond(a, b, c); }
int32_t  psbXSysSignalCond(int32_t a)
    { return sceKernelSignalCond(a); }
int32_t  psbXSysSignalCondAll(int32_t a)
    { return sceKernelSignalCondAll(a); }
int64_t  psbXSysWrite(int32_t a, const void* b, size_t c)
    { return sceKernelWrite(a, b, c); }
int64_t  psbXSysRead(int32_t a, void* b, size_t c)
    { return sceKernelRead(a, b, c); }
int32_t  psbXSysOpen(const char* a, int32_t b, uint32_t c)
    { return sceKernelOpen(a, b, c); }
int32_t  psbXSysClose(int32_t a)
    { return sceKernelClose(a); }
int64_t  psbXSysLseek(int32_t a, int64_t b, int32_t c)
    { return sceKernelLseek(a, b, c); }
int32_t  psbXSysFstat(int32_t a, PsbStat* b)
    { return sceKernelFstat(a, b); }
int32_t  psbXSysFreeDirectMemory(int64_t a, size_t b)
    { return sceKernelFreeDirectMemory(a, b); }
int32_t  psbXSysMapNamedDirectMemory(void** a, size_t b, int c, int d, int64_t e, size_t f, const char* g)
    { return sceKernelMapNamedDirectMemory(a, b, c, d, e, f, g); }
int32_t  psbXSysMmap(void** a, size_t b, int c, int d, int32_t e, int64_t f)
    { return sceKernelMmap(a, b, c, d, e, f); }
int32_t  psbXSysMunmap(void* a, size_t b)
    { return sceKernelMunmap(a, b); }
int32_t  psbXSysQueryMemoryProtection(void* a, void** b, void** c, int32_t* d)
    { return sceKernelQueryMemoryProtection(a, b, c, d); }
int32_t  psbXSysCreateEqueue(const char* a, int32_t* b)
    { return sceKernelCreateEqueue(a, b); }
int32_t  psbXSysDeleteEqueue(int32_t a)
    { return sceKernelDeleteEqueue(a); }
int32_t  psbXSysAddEvent(int32_t a, uint64_t b, uint32_t c, uint64_t d)
    { return sceKernelAddEvent(a, b, c, d); }
int32_t  psbXSysTriggerEvent(int32_t a, uint64_t b, uint32_t c, uint64_t d)
    { return sceKernelTriggerEvent(a, b, c, d); }
int32_t  psbXSysWaitEqueue(int32_t a, PsbEqueueEvent* b, uint64_t c)
    { return sceKernelWaitEqueue(a, b, c); }
int32_t  psbXSysCancelEvent(int32_t a, uint64_t b)
    { return sceKernelCancelEvent(a, b); }
int32_t  psbXSysIsNeoMode(void)
    { return sceKernelIsNeoMode(); }
uint64_t psbXProbeCall1(uint64_t a)
    { return psbProbeCall1(a); }
uint64_t psbXProbeCall2(uint64_t a, uint64_t b)
    { return psbProbeCall2(a, b); }
uint64_t psbXProbeCall3(uint64_t a, uint64_t b, uint64_t c)
    { return psbProbeCall3(a, b, c); }
uint64_t psbXProbeCall4(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
    { return psbProbeCall4(a, b, c, d); }
uint64_t psbXProbeCall5(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
    { return psbProbeCall5(a, b, c, d, e); }
uint64_t psbXProbeCall6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
    { return psbProbeCall6(a, b, c, d, e, f); }
int32_t  psbXProbePtr2(void* a, uint64_t b)
    { return psbProbePtr2(a, b); }
void*    psbXTlsGetAddr(PsbTlsIndex* a)
    { return psbTlsGetAddr(a); }
void     psbXCxaFinalize(void* a)
    { psbCxaFinalize(a); }
int32_t  psbXGetRuntimeVersion(void)
    { return psbGetRuntimeVersion(); }
const char* psbXGetRuntimeName(void)
    { return psbGetRuntimeName(); }
void     psbXStackChkFail(void)
    { psbStackChkFail(); }
} // extern "C"
#endif // _WIN32

// ---- PSBET runtime --------------------------------------------------------
// NOTE: implementations are deliberately NOT named __tls_get_addr /
// __cxa_finalize in the host symbol table — defining those symbols would
// interpose glibc's own for the whole host process (ASan and libstdc++
// depend on the real ones). The guest-facing names exist only in the export
// table below, which is what the ELF loader binds guest GOT entries to.
void* psbTlsGetAddr(PsbTlsIndex* idx) {
    return KernelState::get().tlsGetAddr(idx);
}

void psbCxaFinalize(void* /*d*/) {
    // Guest images built with -nostartfiles may reference this; no-op.
}

// GCC's stack protector emits calls to __stack_chk_fail in guest images
// built without -fno-stack-protector. Provide a host-side implementation so
// such images still load; a triggered check means guest-stack corruption.
void psbStackChkFail(void) {
    LOG_ERROR("kernel: guest stack smashing detected (__stack_chk_fail)");
    std::quick_exit(139);
}

int32_t psbGetRuntimeVersion(void) {
    return static_cast<int32_t>((kVersionMajor << 16) | (kVersionMinor << 8)
                              | kVersionPatch);
}

const char* psbGetRuntimeName(void) {
    static const char* name = [] {
        static char buf[64];
        std::snprintf(buf, sizeof(buf), "PSBET %u.%u.%u (clean-room PS5 runtime)",
                      kVersionMajor, kVersionMinor, kVersionPatch);
        return buf;
    }();
    return name;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Export table
// ---------------------------------------------------------------------------
namespace {

struct Export { const char* name; void* fn; };

#if defined(_WIN32)
// Windows: guests speak SysV; the MASM thunks (w64_thunks.asm) shuffle into
// Win64 and forward to the psbX* forwarders above.
extern "C" {
void psbTWinAllocateDirectMemory(void);  void psbTWinMapDirectMemory(void);
void psbTWinGetDirectMemorySize(void);   void psbTWinGetProcessTime(void);
void psbTWinGetProcessTimeInTick(void);  void psbTWinUsleep(void);
void psbTWinNanosleep(void);             void psbTWinSleep(void);
void psbTWinCreateThread(void);          void psbTWinStartThread(void);
void psbTWinWaitThreadEnd(void);         void psbTWinExitThread(void);
void psbTWinExitProcess(void);           void psbTWinGetThreadId(void);
void psbTWinCreateMutex(void);           void psbTWinLockMutex(void);
void psbTWinUnlockMutex(void);           void psbTWinCreateSemaphore(void);
void psbTWinWaitSemaphore(void);         void psbTWinSignalSemaphore(void);
void psbTWinCreateCond(void);            void psbTWinWaitCond(void);
void psbTWinSignalCond(void);            void psbTWinSignalCondAll(void);
void psbTWinWrite(void);                 void psbTWinRead(void);
void psbTWinOpen(void);                  void psbTWinClose(void);
void psbTWinLseek(void);                 void psbTWinFstat(void);
void psbTWinFreeDirectMemory(void);      void psbTWinMapNamedDirectMemory(void);
void psbTWinMmap(void);                  void psbTWinMunmap(void);
void psbTWinQueryMemoryProtection(void);
void psbTWinCreateEqueue(void);          void psbTWinDeleteEqueue(void);
void psbTWinAddEvent(void);              void psbTWinTriggerEvent(void);
void psbTWinWaitEqueue(void);            void psbTWinCancelEvent(void);
void psbTWinIsNeoMode(void);             void psbTWinTlsGetAddr(void);
void psbTWinCxaFinalize(void);           void psbTWinGetRuntimeVersion(void);
void psbTWinGetRuntimeName(void);        void psbTWinGnmSubmit(void);
void psbTWinStackChkFail(void);
void psbTWinProbeCall1(void);            void psbTWinProbeCall2(void);
void psbTWinProbeCall3(void);            void psbTWinProbeCall4(void);
void psbTWinProbeCall5(void);            void psbTWinProbeCall6(void);
void psbTWinProbePtr2(void);
// Phase 4: audio / pad / net / counter surface.
void psbTWinGetProcessTimeCounter(void); void psbTWinGetProcessTimeCounterFrequency(void);
void psbTWinAudioOutOpen(void);          void psbTWinAudioOutOutput(void);
void psbTWinAudioOutSetVolume(void);     void psbTWinAudioOutClose(void);
void psbTWinPadOpen(void);               void psbTWinPadReadState(void);
void psbTWinPadRead(void);               void psbTWinPadGetControllerInformation(void);
void psbTWinNetInit(void);               void psbTWinNetTerm(void);
void psbTWinNetSocket(void);             void psbTWinNetConnect(void);
void psbTWinNetBind(void);               void psbTWinNetListen(void);
void psbTWinNetAccept(void);             void psbTWinNetSend(void);
void psbTWinNetRecv(void);               void psbTWinNetCloseSocket(void);
void psbTWinNetCtlInit(void);            void psbTWinNetCtlTerm(void);
void psbTWinNetCtlGetState(void);
}
#define W(name) psbTWin##name
const Export kExports[] = {
    {"sceKernelAllocateDirectMemory", (void*)&W(AllocateDirectMemory)},
    {"sceKernelMapDirectMemory",      (void*)&W(MapDirectMemory)},
    {"sceKernelGetDirectMemorySize",  (void*)&W(GetDirectMemorySize)},
    {"sceKernelGetProcessTime",       (void*)&W(GetProcessTime)},
    {"sceKernelGetProcessTimeInTick", (void*)&W(GetProcessTimeInTick)},
    {"sceKernelGetProcessTimeCounter",(void*)&W(GetProcessTimeCounter)},
    {"sceKernelGetProcessTimeCounterFrequency",
                                      (void*)&W(GetProcessTimeCounterFrequency)},
    {"sceKernelUsleep",               (void*)&W(Usleep)},
    {"sceKernelNanosleep",            (void*)&W(Nanosleep)},
    {"sceKernelSleep",                (void*)&W(Sleep)},
    {"sceKernelCreateThread",         (void*)&W(CreateThread)},
    {"sceKernelStartThread",          (void*)&W(StartThread)},
    {"sceKernelWaitThreadEnd",        (void*)&W(WaitThreadEnd)},
    {"sceKernelExitThread",           (void*)&W(ExitThread)},
    {"sceKernelExitProcess",          (void*)&W(ExitProcess)},
    {"sceKernelGetThreadId",          (void*)&W(GetThreadId)},
    {"sceKernelCreateMutex",          (void*)&W(CreateMutex)},
    {"sceKernelLockMutex",            (void*)&W(LockMutex)},
    {"sceKernelUnlockMutex",          (void*)&W(UnlockMutex)},
    {"sceKernelCreateSemaphore",      (void*)&W(CreateSemaphore)},
    {"sceKernelWaitSemaphore",        (void*)&W(WaitSemaphore)},
    {"sceKernelSignalSemaphore",      (void*)&W(SignalSemaphore)},
    {"sceKernelCreateCond",           (void*)&W(CreateCond)},
    {"sceKernelWaitCond",             (void*)&W(WaitCond)},
    {"sceKernelSignalCond",           (void*)&W(SignalCond)},
    {"sceKernelSignalCondAll",        (void*)&W(SignalCondAll)},
    {"sceKernelWrite",                (void*)&W(Write)},
    {"sceKernelRead",                 (void*)&W(Read)},
    {"sceKernelOpen",                 (void*)&W(Open)},
    {"sceKernelClose",                (void*)&W(Close)},
    {"sceKernelLseek",                (void*)&W(Lseek)},
    {"sceKernelFstat",                (void*)&W(Fstat)},
    {"sceKernelFreeDirectMemory",     (void*)&W(FreeDirectMemory)},
    {"sceKernelMapNamedDirectMemory", (void*)&W(MapNamedDirectMemory)},
    {"sceKernelMmap",                 (void*)&W(Mmap)},
    {"sceKernelMunmap",               (void*)&W(Munmap)},
    {"sceKernelQueryMemoryProtection",(void*)&W(QueryMemoryProtection)},
    {"sceKernelCreateEqueue",         (void*)&W(CreateEqueue)},
    {"sceKernelDeleteEqueue",         (void*)&W(DeleteEqueue)},
    {"sceKernelAddEvent",             (void*)&W(AddEvent)},
    {"sceKernelTriggerEvent",         (void*)&W(TriggerEvent)},
    {"sceKernelWaitEqueue",           (void*)&W(WaitEqueue)},
    {"sceKernelCancelEvent",          (void*)&W(CancelEvent)},
    {"sceKernelIsNeoMode",            (void*)&W(IsNeoMode)},
    {"__stack_chk_fail",              (void*)&W(StackChkFail)},
    {"__tls_get_addr",                (void*)&W(TlsGetAddr)},
    {"__cxa_finalize",                (void*)&W(CxaFinalize)},
    {"psbGetRuntimeVersion",          (void*)&W(GetRuntimeVersion)},
    {"psbGetRuntimeName",             (void*)&W(GetRuntimeName)},
    {"psbProbeCall1",                 (void*)&W(ProbeCall1)},
    {"psbProbeCall2",                 (void*)&W(ProbeCall2)},
    {"psbProbeCall3",                 (void*)&W(ProbeCall3)},
    {"psbProbeCall4",                 (void*)&W(ProbeCall4)},
    {"psbProbeCall5",                 (void*)&W(ProbeCall5)},
    {"psbProbeCall6",                 (void*)&W(ProbeCall6)},
    {"psbProbePtr2",                  (void*)&W(ProbePtr2)},
    // Phase 4: audio out / pad / net (thunks live in w64_thunks.asm; the
    // psbXAudio*/psbXPad*/psbXNet* forwarders live in their modules).
    {"sceAudioOutOpen",               (void*)&W(AudioOutOpen)},
    {"sceAudioOutOutput",             (void*)&W(AudioOutOutput)},
    {"sceAudioOutSetVolume",          (void*)&W(AudioOutSetVolume)},
    {"sceAudioOutClose",              (void*)&W(AudioOutClose)},
    {"scePadOpen",                    (void*)&W(PadOpen)},
    {"scePadReadState",               (void*)&W(PadReadState)},
    {"scePadRead",                    (void*)&W(PadRead)},
    {"scePadGetControllerInformation",(void*)&W(PadGetControllerInformation)},
    {"sceNetInit",                    (void*)&W(NetInit)},
    {"sceNetTerm",                    (void*)&W(NetTerm)},
    {"sceNetSocket",                  (void*)&W(NetSocket)},
    {"sceNetConnect",                 (void*)&W(NetConnect)},
    {"sceNetBind",                    (void*)&W(NetBind)},
    {"sceNetListen",                  (void*)&W(NetListen)},
    {"sceNetAccept",                  (void*)&W(NetAccept)},
    {"sceNetSend",                    (void*)&W(NetSend)},
    {"sceNetRecv",                    (void*)&W(NetRecv)},
    {"sceNetCloseSocket",             (void*)&W(NetCloseSocket)},
    {"sceNetCtlInit",                 (void*)&W(NetCtlInit)},
    {"sceNetCtlTerm",                 (void*)&W(NetCtlTerm)},
    {"sceNetCtlGetState",             (void*)&W(NetCtlGetState)},
};
#undef W
#else
const Export kExports[] = {
    {"sceKernelAllocateDirectMemory", (void*)&sceKernelAllocateDirectMemory},
    {"sceKernelMapDirectMemory",      (void*)&sceKernelMapDirectMemory},
    {"sceKernelGetDirectMemorySize",  (void*)&sceKernelGetDirectMemorySize},
    {"sceKernelGetProcessTime",       (void*)&sceKernelGetProcessTime},
    {"sceKernelGetProcessTimeInTick", (void*)&sceKernelGetProcessTimeInTick},
    {"sceKernelGetProcessTimeCounter",(void*)&sceKernelGetProcessTimeCounter},
    {"sceKernelGetProcessTimeCounterFrequency",
                                      (void*)&sceKernelGetProcessTimeCounterFrequency},
    {"sceKernelUsleep",               (void*)&sceKernelUsleep},
    {"sceKernelNanosleep",            (void*)&sceKernelNanosleep},
    {"sceKernelSleep",                (void*)&sceKernelSleep},
    {"sceKernelCreateThread",         (void*)&sceKernelCreateThread},
    {"sceKernelStartThread",          (void*)&sceKernelStartThread},
    {"sceKernelWaitThreadEnd",        (void*)&sceKernelWaitThreadEnd},
    {"sceKernelExitThread",           (void*)&sceKernelExitThread},
    {"sceKernelExitProcess",          (void*)&sceKernelExitProcess},
    {"sceKernelGetThreadId",          (void*)&sceKernelGetThreadId},
    {"sceKernelCreateMutex",          (void*)&sceKernelCreateMutex},
    {"sceKernelLockMutex",            (void*)&sceKernelLockMutex},
    {"sceKernelUnlockMutex",          (void*)&sceKernelUnlockMutex},
    {"sceKernelCreateSemaphore",      (void*)&sceKernelCreateSemaphore},
    {"sceKernelWaitSemaphore",        (void*)&sceKernelWaitSemaphore},
    {"sceKernelSignalSemaphore",      (void*)&sceKernelSignalSemaphore},
    {"sceKernelCreateCond",           (void*)&sceKernelCreateCond},
    {"sceKernelWaitCond",             (void*)&sceKernelWaitCond},
    {"sceKernelSignalCond",           (void*)&sceKernelSignalCond},
    {"sceKernelSignalCondAll",        (void*)&sceKernelSignalCondAll},
    {"sceKernelWrite",                (void*)&sceKernelWrite},
    {"sceKernelRead",                 (void*)&sceKernelRead},
    {"sceKernelOpen",                 (void*)&sceKernelOpen},
    {"sceKernelClose",                (void*)&sceKernelClose},
    {"sceKernelLseek",                (void*)&sceKernelLseek},
    {"sceKernelFstat",                (void*)&sceKernelFstat},
    {"sceKernelFreeDirectMemory",     (void*)&sceKernelFreeDirectMemory},
    {"sceKernelMapNamedDirectMemory", (void*)&sceKernelMapNamedDirectMemory},
    {"sceKernelMmap",                 (void*)&sceKernelMmap},
    {"sceKernelMunmap",               (void*)&sceKernelMunmap},
    {"sceKernelQueryMemoryProtection",(void*)&sceKernelQueryMemoryProtection},
    {"sceKernelCreateEqueue",         (void*)&sceKernelCreateEqueue},
    {"sceKernelDeleteEqueue",         (void*)&sceKernelDeleteEqueue},
    {"sceKernelAddEvent",             (void*)&sceKernelAddEvent},
    {"sceKernelTriggerEvent",         (void*)&sceKernelTriggerEvent},
    {"sceKernelWaitEqueue",           (void*)&sceKernelWaitEqueue},
    {"sceKernelCancelEvent",          (void*)&sceKernelCancelEvent},
    {"sceKernelIsNeoMode",            (void*)&sceKernelIsNeoMode},
    {"__stack_chk_fail",              (void*)&psbStackChkFail},
    {"__tls_get_addr",                (void*)&psbTlsGetAddr},
    {"__cxa_finalize",                (void*)&psbCxaFinalize},
    {"psbGetRuntimeVersion",          (void*)&psbGetRuntimeVersion},
    {"psbGetRuntimeName",             (void*)&psbGetRuntimeName},
    {"psbProbeCall1",                 (void*)&psbProbeCall1},
    {"psbProbeCall2",                 (void*)&psbProbeCall2},
    {"psbProbeCall3",                 (void*)&psbProbeCall3},
    {"psbProbeCall4",                 (void*)&psbProbeCall4},
    {"psbProbeCall5",                 (void*)&psbProbeCall5},
    {"psbProbeCall6",                 (void*)&psbProbeCall6},
    {"psbProbePtr2",                  (void*)&psbProbePtr2},
    // Phase 4: audio out / pad / net (defined in their engine modules).
    {"sceAudioOutOpen",               (void*)&psb::audio::sceAudioOutOpen},
    {"sceAudioOutOutput",             (void*)&psb::audio::sceAudioOutOutput},
    {"sceAudioOutSetVolume",          (void*)&psb::audio::sceAudioOutSetVolume},
    {"sceAudioOutClose",              (void*)&psb::audio::sceAudioOutClose},
    {"scePadOpen",                    (void*)&scePadOpen},
    {"scePadReadState",               (void*)&scePadReadState},
    {"scePadRead",                    (void*)&scePadRead},
    {"scePadGetControllerInformation",(void*)&scePadGetControllerInformation},
    {"sceNetInit",                    (void*)&sceNetInit},
    {"sceNetTerm",                    (void*)&sceNetTerm},
    {"sceNetSocket",                  (void*)&sceNetSocket},
    {"sceNetConnect",                 (void*)&sceNetConnect},
    {"sceNetBind",                    (void*)&sceNetBind},
    {"sceNetListen",                  (void*)&sceNetListen},
    {"sceNetAccept",                  (void*)&sceNetAccept},
    {"sceNetSend",                    (void*)&sceNetSend},
    {"sceNetRecv",                    (void*)&sceNetRecv},
    {"sceNetCloseSocket",             (void*)&sceNetCloseSocket},
    {"sceNetCtlInit",                 (void*)&sceNetCtlInit},
    {"sceNetCtlTerm",                 (void*)&sceNetCtlTerm},
    {"sceNetCtlGetState",             (void*)&sceNetCtlGetState},
};
#endif // _WIN32

} // namespace

void* KernelState::find(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kExports)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

} // namespace psb::kernel
