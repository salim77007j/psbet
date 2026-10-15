// ============================================================================
//  PSBET — memory.h
//  Guest address-space manager.
//
//  Phase 1 model (documented deviation from real PS5):
//   * One contiguous guest arena is reserved in the host address space.
//   * "Direct memory" allocations (the PS5-style physical pool that games
//     map for GPU/CPU sharing) are handed out from this arena with a bump
//     allocator and are immediately CPU-visible — a unified address space.
//   * Executable images, guest stacks, framebuffers and command buffers all
//     live here, which keeps Phase 1 pointers trivially valid while the real
//     PS5 page-table emulation lands in Phase 3.
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>
#include "psbet/common.h"
#include "platform/vm.h"

namespace psb {

class MemoryManager {
public:
    // Guest arena base hint: high VA, far away from the host image and heap.
    static constexpr u64 kArenaHint = 0x0000040000000000ull;
    static constexpr u64 kArenaSize = 16ull * GiB;

    MemoryManager() = default;
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Reserve the arena. Returns false if the OS refuses (fatal).
    bool init();

    // Allocate `size` bytes with `alignment` from the guest arena.
    // Returns guest (= host) virtual address, or 0 on exhaustion.
    u64 allocate(size_t size, size_t alignment, const char* name, vm::Prot prot);

    // Change protection of an allocated range. Page-aligned internally.
    bool protectRange(u64 addr, size_t size, vm::Prot prot);

    // Human-readable allocation map (for logging / crash reports).
    std::string regionMap() const;

    u64 arenaBase() const { return reinterpret_cast<u64>(arena_); }
    u64 bytesAllocated() const;

private:
    struct Region {
        u64 base;
        u64 size;
        std::string name;
    };

    u8*          arena_   = nullptr;
    u64          arenaCap_ = kArenaSize;
    u64          bump_    = 0;      // next free offset inside arena
    mutable std::mutex mtx_;
    std::vector<Region> regions_;
};

} // namespace psb
