// ============================================================================
//  PSBET — kernel_shim.h
//  libkernel emulation: PS5-style kernel surface (threads, sync, memory,
//  time, I/O) implemented on host primitives. This is PSBET's clean-room
//  guest ABI v1 — modelled on Orbis semantics, simplified where noted in
//  docs/GUEST_ABI.md.
// ============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "psbet/common.h"

namespace psb {

class MemoryManager;

namespace kernel {

// Thread entry ABI (Orbis-style: argc/argv replaced by arglen/argp).
using PsbThreadEntry = i32 (*)(size_t arglen, void* argp);

// TLS descriptor layout expected by the x86-64 general-dynamic TLS model.
struct PsbTlsIndex {
    u64 module;
    u64 offset;
};

// ---------------------------------------------------------------------------
// KernelState — handle tables + process-wide state.
// ---------------------------------------------------------------------------
class KernelState {
public:
    static KernelState& get();

    void init(MemoryManager* mem);

    // Export-table lookup used by the ELF loader's import resolver.
    void* find(const char* name);

    // --- TLS module registry (loaded images register themselves) ----------
    void registerTlsModule(u32 id, const void* image, u64 memSize);
    void* tlsGetAddr(PsbTlsIndex* idx);

    // --- Process exit (thread-correct cooperative protocol) --------------
    //
    // Phase-1 flaw fixed: a worker thread calling ExitProcess used to longjmp
    // on the MAIN thread's jmp_buf (cross-thread longjmp = UB). Protocol now:
    //   * main thread (supervisor) calls it → same-thread longjmp, as before;
    //   * a worker calls it → the exit is recorded and the worker unwinds its
    //     OWN guest frames via its per-thread exit env (same path as
    //     ExitThread), so it becomes joinable instead of parked forever;
    //   * blocking kernel primitives wake when exit is requested, so a main
    //     thread waiting on workers returns and finishes the exit;
    //   * the supervisor reads the recorded code when guest execution ends.
    void requestProcessExit(i32 exitCode);
    bool exitRequested() const;
    i32  exitCode() const;

    // --- Thread cleanup at process end (cooperative) -----------------------
    // Wakes everything blocked in kernel primitives, joins finished threads,
    // and detaches only genuinely compute-bound stragglers (logged).
    void shutdownThreads();

    // --- Process start time (for GetProcessTime) --------------------------
    void setProcessStart(u64 usSinceEpochSteady);
    u64  processTimeUs() const;

    MemoryManager* memory() const { return mem_; }

    // Internal: allocate a 1-based handle slot for an object of `kind`.
    i32 addHandle(std::shared_ptr<void> obj, const char* kind);
    std::shared_ptr<void> getHandle(i32 id, const char* kind);

private:
    KernelState() = default;

    MemoryManager* mem_ = nullptr;
    mutable std::mutex mtx_;
    std::vector<std::shared_ptr<void>> handles_;
    std::vector<std::string> handleKinds_;

    struct TlsModule { std::vector<u8> image; u64 memSize = 0; };
    TlsModule tlsModule_;
    bool tlsModuleValid_ = false;

    bool exitRequested_ = false;
    i32  exitCode_ = 0;

    u64 startUs_ = 0;
};

// Called at the top of every guest thread trampoline (and the main thread).
void installThreadTls();

// Process exit plumbing (used by CPU supervisor).
void setProcessJmpBuf(void* env);   // opaque jmp_buf*
void* processJmpBuf();

// Marks the calling thread as the CPU supervisor for this guest run. Must be
// set before guest entry and cleared after (RAII in runGuestEntry).
void beginSupervisorScope();
void endSupervisorScope();

// True when called from the thread that owns the process jmp_buf (the CPU
// supervisor's main thread). Workers get the cooperative path instead.
bool isSupervisorThread();

// Cooperative shutdown flag: kernel primitives wake on this (and on
// exit-requested) so no guest thread is ever parked forever.
void requestShutdown();
bool shutdownRequested();

} // namespace kernel
} // namespace psb
