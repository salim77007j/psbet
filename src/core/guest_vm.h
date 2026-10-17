// ============================================================================
//  PSBET — guest_vm.h
//  Phase-3 memory model: typed direct-memory pools, guest page tables and a
//  separate GPU virtual address space.
//
//  Model contract (see docs/ARCHITECTURE.md §11):
//   * PSBET executes guests natively: a guest CPU pointer IS a host pointer.
//     The CPU virtual address space is therefore pinned to the guest arena
//     and direct-memory CPU mappings are identity (host VA = arena + PA).
//   * The *model* beneath is faithful: physical memory is typed into pools,
//     allocated from free lists honouring Orbis-style search ranges, every
//     mapped page is tracked by a real two-level page table (4 KiB leaves
//     under 2 MiB mid-pages), and the GPU owns a fully separate virtual
//     address space with its own page tables — GPU VAs are translated per
//     access through the walk, never assumed identity.
//   * CPU-side VA!=PA remapping (aliasing a physical allocation at several
//     CPU VAs) is rejected in v0.3 — it buys nothing until framebuffer
//     aliasing work (Phase 5) and would break the native-pointer invariant.
//     Documented divergence, enforced loudly.
// ============================================================================
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "psbet/common.h"
#include "platform/vm.h"

namespace psb {

// ---------------------------------------------------------------------------
// PageTable — two-level guest page table (4 KiB leaves under 2 MiB mid-pages).
//
//  VA bits [47:47] ... (16 GiB window => mid index needs 13 bits, leaf 9 bits)
//    mid  = va >> 21          (0 .. 8191 for a 16 GiB space)
//    leaf = (va >> 12) & 511
//
//  PTE (PSBET-defined, clean-room):
//    bit 0      : present
//    bits 2..1  : protection (vm::Prot low bits: R=1, W=2, X=4)
//    bits 63:12 : physical frame number (4 KiB)
// ---------------------------------------------------------------------------
class PageTable {
public:
    // 16 GiB window (kSpaceSize = 8192 mids x 2 MiB)
    static constexpr u64 kMidCoverage = 2ull * MiB;
    static constexpr u64 kMaxMids     = 8192;
    static constexpr u64 kSpaceSize   = kMidCoverage * kMaxMids;

    PageTable() = default;
    ~PageTable();

    // Set PTEs for [va, va+len) — all addresses 4 KiB aligned, within window.
    bool map(u64 va, u64 len, u64 pa, vm::Prot prot);
    // Clear PTEs for [va, va+len).
    bool unmap(u64 va, u64 len);
    // Walk: translate one page-aligned VA to its PA. Returns false if absent.
    bool translate(u64 va, u64* paOut) const;
    // Protection of one page-aligned VA (0 = not present).
    vm::Prot queryProt(u64 va) const;
    // Update protection of an already-present page. False if not present.
    bool setProt(u64 va, vm::Prot prot);
    // Number of present pages (diagnostics).
    u64 presentPages() const;

private:
    struct Leaf { u64 pte[512]; };
    Leaf* leafFor(u64 va, bool create);
    const Leaf* findLeaf(u64 va) const;
    static u64 makePte(u64 pa, vm::Prot prot);
    static bool ptePresent(u64 pte) { return (pte & 1) != 0; }

    Leaf* mids_[kMaxMids] = {};   // lazily created leaves
    u64   present_ = 0;
};

// ---------------------------------------------------------------------------
// PoolAllocator — free-list physical allocator for one typed memory pool.
// ---------------------------------------------------------------------------
class PoolAllocator {
public:
    struct Range { u64 pa; u64 size; };

    void init(u64 base, u64 size);
    // First-fit allocation of `len` bytes at `alignment` inside
    // [searchStart, searchEnd] (absolute PA; clamped to the pool). 0 = fail.
    u64 alloc(u64 len, u64 alignment, u64 searchStart, u64 searchEnd);
    // Carve an exact range out of the free list (must be fully free).
    bool carve(u64 pa, u64 len);
    // Returns the freed size, or 0 if the range was not an exact live match.
    u64 free(u64 pa, u64 len);
    bool contains(u64 pa, u64 len) const;
    u64  usedBytes() const { return used_; }
    u64  sizeBytes() const { return size_; }

private:
    void coalesce();
    u64  base_ = 0, size_ = 0, used_ = 0;
    std::vector<Range> freeList_;
};

// ---------------------------------------------------------------------------
// GuestMemory — the Phase-3 memory subsystem (pools + CPU page tables +
// GPU VM). One instance per emulator run.
// ---------------------------------------------------------------------------
class GuestMemory {
public:
    // Typed pools (PSBET physical-memory types; Orbis-style type numbers are
    // mapped 1:1 onto these by the kernel shim — see GUEST_ABI §5).
    static constexpr u32 kPoolRegular = 0;   // CPU+GPU general memory
    static constexpr u32 kPoolGpuVisible = 1;// GPU-dominant allocations
    static constexpr u32 kPoolCount   = 2;

    static constexpr u64 kPaRegularSize    = 8ull * GiB;
    static constexpr u64 kPaGpuVisibleSize = 4ull * GiB;
    static constexpr u64 kGpuVaSize        = 8ull * GiB;

    GuestMemory() = default;
    ~GuestMemory();

    bool init();

    // ---- physical (direct memory) -----------------------------------------
    u64  allocDirect(u32 poolId, u64 len, u64 alignment,
                     u64 searchStart, u64 searchEnd, const char* name);
    // Reserve an exact physical range (used by ET_EXEC image loading where
    // guest vaddrs are absolute). Fails when the range is not fully free.
    u64  allocDirectFixed(u32 poolId, u64 pa, u64 len, const char* name);
    bool freeDirect(u64 pa, u64 len);      // refuses while mappings reference it

    // ---- CPU virtual (identity over the pinned arena) ----------------------
    // Map a sub-range of any live physical allocation (the loader maps image
    // segments independently; mapDirect's exact-range lookup is too strict
    // for that). Returns the identity host VA.
    u64  mapDirect(u64 pa, u64 len, vm::Prot prot, const char* name);
    bool unmap(u64 va, u64 len);
    bool protectRange(u64 va, u64 len, vm::Prot prot);

    struct BlockInfo {
        u64  va = 0;
        u64  len = 0;
        vm::Prot prot = vm::Prot::NoAccess;
        u64  pa = 0;
        std::string name;
    };
    bool queryVa(u64 va, BlockInfo* out) const;

    // ---- GPU virtual memory (separate space, real translation) -------------
    u64  gpuReserve(u64 len, u64 alignment, u64 gpuVaFixed); // 0=fail; gpuVaFixed=0 -> pick
    bool gpuMap(u64 gpuVa, u64 pa, u64 len, vm::Prot prot);
    bool gpuUnmap(u64 gpuVa, u64 len);
    // GPU VA -> host pointer (walks the GPU page table). nullptr if unmapped.
    void* gpuTranslate(u64 gpuVa) const;
    // GPU VA -> physical address (page-table walk). False if unmapped.
    bool  gpuTranslateRaw(u64 gpuVa, u64* paOut) const;

    // ---- diagnostics --------------------------------------------------------
    u64  bytesAllocated() const;
    std::string regionMap() const;
    std::string gpuMapDump() const;

    // Host pointer for a physical address (arena + PA), or nullptr.
    void* paHost(u64 pa) const;

private:
    struct PaAlloc { u64 len; u32 pool; u32 mapRefs; std::string name; };

    u8*   arena_ = nullptr;
    u64   arenaCap_ = PageTable::kSpaceSize;   // 16 GiB
    PoolAllocator pools_[kPoolCount];
    std::map<u64, PaAlloc> paAllocs_;          // pa -> record

    struct VaMapping { u64 len; vm::Prot prot; u64 pa; std::string name; };
    std::map<u64, VaMapping> vaMaps_;          // va -> record (identity)

    struct GpuMapping { u64 len; vm::Prot prot; u64 pa; };
    std::map<u64, GpuMapping> gpuMaps_;        // gpuVa -> record
    PoolAllocator gpuVaAlloc_;                 // GPU VA space allocator

    PageTable cpuPt_;
    PageTable gpuPt_;

    mutable std::mutex mtx_;
};

} // namespace psb
