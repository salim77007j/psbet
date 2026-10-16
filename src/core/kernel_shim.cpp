// ============================================================================
//  PSBET — kernel_shim.cpp
//  Guest-visible kernel surface implemented on host primitives.
//
//  Every function here is clean-room work: PS5-style semantics, original
//  implementation, no Sony code or constants.
// ============================================================================
#include "kernel_shim.h"

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
};

struct MutexRec { std::recursive_mutex mtx; };
struct SemaRec {
    std::mutex mtx;
    std::condition_variable cv;
    i64 count = 0;
    i64 max = 0;
};
struct CondRec { std::condition_variable_any cv; };

thread_local std::map<u64, std::vector<u8>> g_tlsBlocks;
thread_local bool   g_inGuestThread = false;
thread_local bool   g_threadExitEnvValid = false;
thread_local i32    g_pendingRetVal = 0;

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
}

// --- cooperative wake state -------------------------------------------------

void requestShutdown() {
    g_shutdown.store(true);
    g_wakeCv.notify_all();
}

bool shutdownRequested() { return g_shutdown.load(); }

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
    std::vector<std::shared_ptr<ThreadRec>> stragglers;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < handles_.size(); ++i) {
            if (handleKinds_[i] != "thread") continue;
            auto rec = std::static_pointer_cast<ThreadRec>(handles_[i]);
            if (!rec->thread.joinable()) continue;
            if (rec->finished) continue;          // joined below in one pass
        }
    }

    // One join pass for finished threads, then a bounded cooperative grace
    // for the rest (they were woken by requestShutdown).
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(200);
    for (;;) {
        bool allDone = true;
        std::vector<std::shared_ptr<ThreadRec>> stillRunning;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (size_t i = 0; i < handles_.size(); ++i) {
                if (handleKinds_[i] != "thread") continue;
                auto rec = std::static_pointer_cast<ThreadRec>(handles_[i]);
                if (!rec->thread.joinable()) continue;
                if (rec->finished || std::chrono::steady_clock::now() > deadline) {
                    stillRunning.push_back(rec);
                } else {
                    allDone = false;
                    stillRunning.push_back(rec);
                }
            }
        }
        for (auto& rec : stillRunning) {
            if (rec->finished && rec->thread.joinable()) rec->thread.join();
        }
        if (allDone || std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Anything still running after the grace period is detached (logged).
    // Compute-bound guest code cannot be preempted portably; this is the
    // documented best-effort boundary.
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < handles_.size(); ++i) {
        if (handleKinds_[i] != "thread") continue;
        auto rec = std::static_pointer_cast<ThreadRec>(handles_[i]);
        if (rec->thread.joinable() && !rec->finished) {
            LOG_WARN("kernel: guest thread '%s' still running at shutdown — detaching",
                     rec->name.c_str());
            rec->thread.detach();
        }
    }
}

void KernelState::setProcessStart(u64 us) {
    std::lock_guard<std::mutex> lk(mtx_);
    startUs_ = us;
}

u64 KernelState::processTimeUs() const {
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<u64>(now) - startUs_;
}

// ---------------------------------------------------------------------------
// Guest-visible functions (extern "C")
// ---------------------------------------------------------------------------
extern "C" {

// ---- memory ---------------------------------------------------------------
int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd,
                                      size_t len, size_t alignment,
                                      int memoryType, int64_t* physAddrOut) {
    (void)searchStart; (void)searchEnd; (void)memoryType;
    KernelState& k = KernelState::get();
    if (!physAddrOut || len == 0) return -1;
    u64 addr = k.memory()->allocate(len, alignment ? alignment : 1,
                                    "direct-memory", vm::Prot::ReadWrite);
    if (!addr) return -1; // Phase 3: typed memory pools
    *physAddrOut = static_cast<int64_t>(addr);
    return 0;
}

int32_t sceKernelMapDirectMemory(void** addrOut, size_t len, int prot, int flags,
                                 int64_t physAddr, size_t pageSizeArg) {
    (void)len; (void)prot; (void)flags; (void)pageSizeArg;
    if (!addrOut || physAddr <= 0) return -1;
    // Unified address space (Phase 1): direct memory is already CPU-visible.
    *addrOut = reinterpret_cast<void*>(static_cast<u64>(physAddr));
    return 0;
}

int32_t sceKernelGetDirectMemorySize(size_t* out) {
    if (!out) return -1;
    *out = static_cast<size_t>(MemoryManager::kArenaSize);
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

int32_t sceKernelUsleep(uint64_t microseconds) {
#if !defined(_WIN32)
    ::usleep(static_cast<useconds_t>(microseconds));
#else
    Sleep(static_cast<DWORD>(microseconds / 1000));
#endif
    return 0;
}

int32_t sceKernelNanosleep(uint64_t seconds, uint64_t nanoseconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds)
                              + std::chrono::nanoseconds(nanoseconds));
    return 0;
}

int32_t sceKernelSleep(uint64_t seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return 0;
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
        installThreadTls();
        if (setjmp(g_threadExitEnv.buf) == 0) {
            g_threadExitEnvValid = true;
            PsbThreadEntry fn = reinterpret_cast<PsbThreadEntry>(raw->entry);
            g_pendingRetVal = fn(raw->argBuf.size(),
                                 raw->argBuf.empty()
                                     ? nullptr
                                     : static_cast<void*>(raw->argBuf.data()));
        }
        // Reached via sceKernelExitThread longjmp or natural return.
        g_threadExitEnvValid = false;
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
        rec->thread.join();
    }
    if (retVal) *retVal = rec->retVal;
    return 0;
}

void sceKernelExitThread(int32_t retVal) {
    if (!g_inGuestThread || !g_threadExitEnvValid) {
        LOG_WARN("kernel: ExitThread outside a guest thread — ignoring");
        return;
    }
    g_pendingRetVal = retVal;
    longjmp(g_threadExitEnv.buf, 1);
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

// ---- I/O ------------------------------------------------------------------
// Phase-1 console-interleaving flaw fixed: guest writes share the engine's
// console mutex and flush at each write, so lines never interleave mid-line
// between guest stdout and host stderr logging.
int64_t sceKernelWrite(int32_t fd, const void* buf, size_t n) {
    if (!buf) return -1;
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

int64_t sceKernelRead(int32_t fd, void* buf, size_t n) {
#if !defined(_WIN32)
    return ::read(fd, buf, n);
#else
    (void)fd; (void)buf; (void)n;
    return -1; // stdin reads arrive with the SDL input phase
#endif
}

int32_t sceKernelOpen(const char* path, int32_t flags, uint32_t mode) {
#if !defined(_WIN32)
    return ::open(path, flags, static_cast<mode_t>(mode));
#else
    // Map the common guest flag subset to _open (read/write/create).
    int m = _O_BINARY;
    if (flags & 0x1) m |= _O_WRONLY;                 // O_WRONLY (subset bit)
    else if (flags & 0x2) m |= _O_RDWR;              // O_RDWR
    else m |= _O_RDONLY;
    if (flags & 0x40) m |= _O_CREAT;                 // O_CREAT
    if (flags & 0x200) m |= _O_TRUNC;                // O_TRUNC
    return ::_open(path, m, static_cast<int>(mode));
#endif
}

int32_t sceKernelClose(int32_t fd) {
#if !defined(_WIN32)
    return ::close(fd);
#else
    return ::_close(fd);
#endif
}

// ---- misc -----------------------------------------------------------------
int32_t sceKernelIsNeoMode(void) {
    return 0; // base model
}

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
    return "PSBET 0.1.0 (clean-room PS5 runtime)";
}

} // extern "C"

// ---------------------------------------------------------------------------
// Export table
// ---------------------------------------------------------------------------
namespace {

struct Export { const char* name; void* fn; };

const Export kExports[] = {
    {"sceKernelAllocateDirectMemory", (void*)&sceKernelAllocateDirectMemory},
    {"sceKernelMapDirectMemory",      (void*)&sceKernelMapDirectMemory},
    {"sceKernelGetDirectMemorySize",  (void*)&sceKernelGetDirectMemorySize},
    {"sceKernelGetProcessTime",       (void*)&sceKernelGetProcessTime},
    {"sceKernelGetProcessTimeInTick", (void*)&sceKernelGetProcessTimeInTick},
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
    {"sceKernelIsNeoMode",            (void*)&sceKernelIsNeoMode},
    {"__stack_chk_fail",              (void*)&psbStackChkFail},
    {"__tls_get_addr",                (void*)&psbTlsGetAddr},
    {"__cxa_finalize",                (void*)&psbCxaFinalize},
    {"psbGetRuntimeVersion",          (void*)&psbGetRuntimeVersion},
    {"psbGetRuntimeName",             (void*)&psbGetRuntimeName},
};

} // namespace

void* KernelState::find(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kExports)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

} // namespace psb::kernel
