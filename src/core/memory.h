// ============================================================================
//  PSBET — memory.h
//  Guest address-space manager (Phase-3 facade).
//
//  The faithful model (typed pools, page tables, GPU VM) lives in guest_vm.h.
//  MemoryManager remains the name the loader, kernel shim and tests know; it
//  now delegates to GuestMemory and exposes the v2 surface alongside the
//  legacy allocate/protectRange API used by the ELF loader.
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>
#include "psbet/common.h"
#include "platform/vm.h"
#include "core/guest_vm.h"

namespace psb {

class MemoryManager {
public:
    // Guest arena base hint: high VA, far away from the host image and heap.
    static constexpr u64 kArenaHint = 0x0000040000000000ull;
    static constexpr u64 kArenaSize = 16ull * GiB;

    MemoryManager() = default;
    ~MemoryManager() = default;

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Reserve the arena. Returns false if the OS refuses (fatal).
    bool init() { return mem_.init(); }

    // ---- legacy facade (used by the ELF loader / internal allocations) -----
    // Allocate + identity-map from the regular pool. Returns guest (= host)
    // virtual address, or 0 on exhaustion.
    u64 allocate(size_t size, size_t alignment, const char* name, vm::Prot prot);

    // Change protection of an allocated range. Page-aligned internally.
    bool protectRange(u64 addr, size_t size, vm::Prot prot)
        { return mem_.protectRange(addr, size, prot); }

    // Human-readable allocation map (for logging / crash reports).
    std::string regionMap() const { return mem_.regionMap(); }

    u64 arenaBase() const { return reinterpret_cast<u64>(mem_.paHost(0)); }
    u64 bytesAllocated() const { return mem_.bytesAllocated(); }

    // ---- Phase-3 faithful surface ------------------------------------------
    GuestMemory& guest() { return mem_; }
    const GuestMemory& guest() const { return mem_; }

private:
    GuestMemory mem_;
};

} // namespace psb
