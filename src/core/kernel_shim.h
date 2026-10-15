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

    // --- Process exit (longjmp-based, CPU supervisor owns the jmp_buf) ----
    void requestProcessExit(i32 exitCode);   // main thread only
    bool exitRequested() const;
    i32  exitCode() const;

    // --- Thread cleanup at process end (detach stragglers) ----------------
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

} // namespace kernel
} // namespace psb
