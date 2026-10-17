// ============================================================================
//  PSBET — guest_vm.cpp
//  Phase-3 memory model implementation. See guest_vm.h for the model contract.
// ============================================================================
#include "guest_vm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "core/log.h"

namespace psb {

// ---------------------------------------------------------------------------
// PageTable
// ---------------------------------------------------------------------------
PageTable::~PageTable() {
    for (Leaf* l : mids_) delete l;
}

u64 PageTable::makePte(u64 pa, vm::Prot prot) {
    return 1ull | (static_cast<u64>(prot) & 7ull) << 1
         | (pa >> 12) << 12;
}

PageTable::Leaf* PageTable::leafFor(u64 va, bool create) {
    const u64 mid = va >> 21;
    if (mid >= kMaxMids) return nullptr;
    Leaf*& slot = mids_[mid];
    if (!slot && create) {
        slot = new Leaf{};
        std::memset(slot, 0, sizeof(Leaf));
    }
    return slot;
}

const PageTable::Leaf* PageTable::findLeaf(u64 va) const {
    const u64 mid = va >> 21;
    if (mid >= kMaxMids) return nullptr;
    return mids_[mid];
}

bool PageTable::map(u64 va, u64 len, u64 pa, vm::Prot prot) {
    if (va % 0x1000 || pa % 0x1000 || len % 0x1000) return false;
    if (va + len > kSpaceSize || pa + len > kSpaceSize) return false;
    for (u64 off = 0; off < len; off += 0x1000) {
        Leaf* leaf = leafFor(va + off, /*create*/true);
        if (!leaf) return false;
        u64& pte = leaf->pte[((va + off) >> 12) & 511];
        if (ptePresent(pte)) return false;   // double-map is a hard error
        pte = makePte(pa + off, prot);
        ++present_;
    }
    return true;
}

bool PageTable::unmap(u64 va, u64 len) {
    if (va % 0x1000 || len % 0x1000) return false;
    if (va + len > kSpaceSize) return false;
    bool ok = true;
    for (u64 off = 0; off < len; off += 0x1000) {
        Leaf* leaf = leafFor(va + off, /*create*/false);
        if (!leaf) { ok = false; continue; }
        u64& pte = leaf->pte[((va + off) >> 12) & 511];
        if (ptePresent(pte)) {
            pte = 0;
            --present_;
        } else {
            ok = false;
        }
    }
    return ok;
}

bool PageTable::translate(u64 va, u64* paOut) const {
    const Leaf* leaf = findLeaf(va);
    if (!leaf) return false;
    const u64 pte = leaf->pte[(va >> 12) & 511];
    if (!ptePresent(pte)) return false;
    if (paOut) *paOut = ((pte >> 12) << 12) | (va & 0xFFF);
    return true;
}

vm::Prot PageTable::queryProt(u64 va) const {
    const Leaf* leaf = findLeaf(va);
    if (!leaf) return vm::Prot::NoAccess;
    const u64 pte = leaf->pte[(va >> 12) & 511];
    if (!ptePresent(pte)) return vm::Prot::NoAccess;
    return static_cast<vm::Prot>((pte >> 1) & 7ull);
}

bool PageTable::setProt(u64 va, vm::Prot prot) {
    Leaf* leaf = leafFor(va, /*create*/false);
    if (!leaf) return false;
    u64& pte = leaf->pte[(va >> 12) & 511];
    if (!ptePresent(pte)) return false;
    pte = (pte & ~(7ull << 1)) | ((static_cast<u64>(prot) & 7ull) << 1);
    return true;
}

u64 PageTable::presentPages() const { return present_; }

// ---------------------------------------------------------------------------
// PoolAllocator
// ---------------------------------------------------------------------------
void PoolAllocator::init(u64 base, u64 size) {
    base_ = base;
    size_ = size;
    used_ = 0;
    freeList_.clear();
    if (size) freeList_.push_back({base, size});
}

void PoolAllocator::coalesce() {
    std::sort(freeList_.begin(), freeList_.end(),
              [](const Range& a, const Range& b) { return a.pa < b.pa; });
    std::vector<Range> merged;
    for (const Range& r : freeList_) {
        if (!merged.empty() && merged.back().pa + merged.back().size == r.pa)
            merged.back().size += r.size;
        else
            merged.push_back(r);
    }
    freeList_.swap(merged);
}

u64 PoolAllocator::alloc(u64 len, u64 alignment, u64 searchStart, u64 searchEnd) {
    if (len == 0) return 0;
    if (alignment == 0) alignment = 1;

    // Clamp the search window to this pool.
    const u64 lo = std::max(searchStart, base_);
    const u64 hi = std::min(searchEnd == 0 ? base_ + size_ : searchEnd,
                            base_ + size_);
    if (lo >= hi) return 0;

    size_t bestIdx = SIZE_MAX;
    u64 bestPa = 0;
    for (size_t i = 0; i < freeList_.size(); ++i) {
        const Range& r = freeList_[i];
        const u64 rLo = std::max(r.pa, lo);
        const u64 start = alignUp(rLo, alignment);
        if (start < r.pa || start < lo) continue;
        if (start + len > r.pa + r.size || start + len > hi) continue;
        if (bestIdx == SIZE_MAX || start < bestPa) {
            bestIdx = i;
            bestPa  = start;
        }
    }
    if (bestIdx == SIZE_MAX) return 0;

    Range& r = freeList_[bestIdx];
    const u64 headLo = r.pa;
    const u64 headSz = bestPa - r.pa;
    const u64 tailPa = bestPa + len;
    const u64 tailSz = r.pa + r.size - tailPa;

    // Replace the consumed range with its leftover head and tail.
    std::vector<Range> repl;
    if (headSz) repl.push_back({headLo, headSz});
    if (tailSz) repl.push_back({tailPa, tailSz});
    freeList_.erase(freeList_.begin() + bestIdx);
    for (const Range& nr : repl) freeList_.push_back(nr);
    used_ += len;
    return bestPa;
}

bool PoolAllocator::carve(u64 pa, u64 len) {
    if (!contains(pa, len) || len == 0) return false;
    for (size_t i = 0; i < freeList_.size(); ++i) {
        const Range& r = freeList_[i];
        if (pa >= r.pa && pa + len <= r.pa + r.size) {
            const u64 headSz = pa - r.pa;
            const u64 tailPa = pa + len;
            const u64 tailSz = r.pa + r.size - tailPa;
            std::vector<Range> repl;
            if (headSz) repl.push_back({r.pa, headSz});
            if (tailSz) repl.push_back({tailPa, tailSz});
            freeList_.erase(freeList_.begin() + i);
            for (const Range& nr : repl) freeList_.push_back(nr);
            used_ += len;
            return true;
        }
    }
    return false;
}

u64 PoolAllocator::free(u64 pa, u64 len) {
    if (!contains(pa, len) || len == 0) return 0;
    // Overlap with an existing free range means double-free / bad args.
    for (const Range& r : freeList_) {
        if (pa < r.pa + r.size && r.pa < pa + len) return 0;
    }
    freeList_.push_back({pa, len});
    coalesce();
    used_ = (used_ > len) ? used_ - len : 0;
    return len;
}

bool PoolAllocator::contains(u64 pa, u64 len) const {
    return len > 0 && pa >= base_ && pa + len <= base_ + size_;
}

// ---------------------------------------------------------------------------
// GuestMemory
// ---------------------------------------------------------------------------
GuestMemory::~GuestMemory() {
    if (arena_) vm::release(arena_, arenaCap_);
}

bool GuestMemory::init() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (arena_) return true;

    static constexpr u64 kHint = 0x0000040000000000ull;
    void* p = vm::reserve(arenaCap_, reinterpret_cast<void*>(kHint));
    if (!p) p = vm::reserve(arenaCap_, nullptr);
    if (!p) {
        LOG_ERROR("GuestMemory: failed to reserve %llu bytes",
                  static_cast<unsigned long long>(arenaCap_));
        return false;
    }
    arena_ = static_cast<u8*>(p);

    // PA 0 is reserved as the null sentinel (never allocatable): guests that
    // dereference a null pointer hit an unmapped page and die loudly instead
    // of silently corrupting the first physical page.
    pools_[kPoolRegular].init(0x1000, kPaRegularSize - 0x1000);
    pools_[kPoolGpuVisible].init(kPaRegularSize, kPaGpuVisibleSize);
    gpuVaAlloc_.init(0x1000, kGpuVaSize - 0x1000);

    LOG_INFO("GuestMemory: PA/VA arena reserved at 0x%llx (%llu MiB); pools: "
             "regular=%llu MiB, gpu-visible=%llu MiB; GPU VA space=%llu MiB",
             static_cast<unsigned long long>(reinterpret_cast<u64>(arena_)),
             static_cast<unsigned long long>(arenaCap_ / MiB),
             static_cast<unsigned long long>(kPaRegularSize / MiB),
             static_cast<unsigned long long>(kPaGpuVisibleSize / MiB),
             static_cast<unsigned long long>(kGpuVaSize / MiB));
    return true;
}

void* GuestMemory::paHost(u64 pa) const {
    if (pa >= arenaCap_) return nullptr;
    return arena_ + pa;
}

u64 GuestMemory::allocDirectFixed(u32 poolId, u64 pa, u64 len, const char* name) {
    if (poolId >= kPoolCount || len == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    // Overlap check against live allocations in every pool.
    for (const auto& [existing, a] : paAllocs_) {
        if (pa < existing + a.len && existing < pa + len) return 0;
    }
    if (!pools_[poolId].contains(pa, len)) return 0;
    if (!pools_[poolId].carve(pa, len)) return 0;
    paAllocs_[pa] = {len, poolId, 0, name ? name : "direct-fixed"};
    return pa;
}

u64 GuestMemory::allocDirect(u32 poolId, u64 len, u64 alignment,
                             u64 searchStart, u64 searchEnd, const char* name) {
    if (poolId >= kPoolCount) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    const u64 pa = pools_[poolId].alloc(len, alignment, searchStart, searchEnd);
    if (!pa) {
        LOG_ERROR("GuestMemory: pool %u exhausted (need %llu bytes)",
                  poolId, static_cast<unsigned long long>(len));
        return 0;
    }
    paAllocs_[pa] = {len, poolId, 0, name ? name : "direct-memory"};
    return pa;
}

bool GuestMemory::freeDirect(u64 pa, u64 len) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = paAllocs_.find(pa);
    if (it == paAllocs_.end() || it->second.len != len) {
        LOG_ERROR("GuestMemory: freeDirect of unknown range pa=0x%llx len=%llu",
                  static_cast<unsigned long long>(pa),
                  static_cast<unsigned long long>(len));
        return false;
    }
    if (it->second.mapRefs != 0) {
        LOG_ERROR("GuestMemory: freeDirect refused — %u live mapping(s) "
                  "reference pa=0x%llx (unmap first)",
                  it->second.mapRefs, static_cast<unsigned long long>(pa));
        return false;
    }
    if (!pools_[it->second.pool].free(pa, len)) return false;
    paAllocs_.erase(it);
    return true;
}

u64 GuestMemory::mapDirect(u64 pa, u64 len, vm::Prot prot, const char* name) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Find the live allocation that fully contains [pa, pa+len).
    auto it = paAllocs_.upper_bound(pa);
    bool found = false;
    if (it != paAllocs_.begin()) {
        --it;
        found = pa >= it->first && pa + len <= it->first + it->second.len;
    }
    if (!found) {
        LOG_ERROR("GuestMemory: mapDirect of unallocated range pa=0x%llx len=%llu",
                  static_cast<unsigned long long>(pa),
                  static_cast<unsigned long long>(len));
        return 0;
    }
    // Native-execution model: CPU mapping is identity over the pinned arena.
    // PTEs are page-granular; sub-page mappings are idempotent per page when
    // they resolve to the same physical frame (identity model: pa == frame).
    const u64 paPage = pa & ~0xFFFull;
    const u64 paEnd  = alignUp(pa + len, 0x1000);
    const u64 va = reinterpret_cast<u64>(arena_) + paPage;
    for (u64 p = paPage; p < paEnd; p += 0x1000) {
        u64 existing = 0;
        if (cpuPt_.translate(p, &existing)) {
            if (existing != p) {
                LOG_ERROR("GuestMemory: page-table conflict at pa=0x%llx",
                          static_cast<unsigned long long>(p));
                return 0;
            }
            cpuPt_.setProt(p, prot);
        } else if (!cpuPt_.map(p, 0x1000, p, prot)) {
            LOG_ERROR("GuestMemory: page-table map failed for pa=0x%llx",
                      static_cast<unsigned long long>(p));
            return 0;
        }
    }
    if (!vm::commit(reinterpret_cast<void*>(va), paEnd - paPage, prot)) {
        LOG_ERROR("GuestMemory: host commit failed for va=0x%llx",
                  static_cast<unsigned long long>(va));
        return 0;
    }
    ++it->second.mapRefs;
    vaMaps_[reinterpret_cast<u64>(arena_) + pa] = {len, prot, pa,
                                                   name ? name : "mapped"};
    return reinterpret_cast<u64>(arena_) + pa;
}

bool GuestMemory::unmap(u64 va, u64 len) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = vaMaps_.find(va);
    if (it == vaMaps_.end() || it->second.len != len) return false;
    // Page-granular teardown (sub-page unmaps tear whole pages; documented).
    const u64 paPage = it->second.pa & ~0xFFFull;
    const u64 paEnd  = alignUp(it->second.pa + len, 0x1000);
    vm::protect(reinterpret_cast<void*>(reinterpret_cast<u64>(arena_) + paPage),
                paEnd - paPage, vm::Prot::NoAccess);
    cpuPt_.unmap(paPage, paEnd - paPage);
    auto paIt = paAllocs_.find(it->second.pa);
    if (paIt == paAllocs_.end()) {
        // Sub-range of a larger allocation: find its owner for the refcount.
        auto owner = paAllocs_.upper_bound(it->second.pa);
        if (owner != paAllocs_.begin()) {
            --owner;
            if (owner->second.mapRefs > 0) --owner->second.mapRefs;
        }
    } else if (paIt->second.mapRefs > 0) {
        --paIt->second.mapRefs;
    }
    vaMaps_.erase(it);
    return true;
}

bool GuestMemory::protectRange(u64 va, u64 len, vm::Prot prot) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (va + len > reinterpret_cast<u64>(arena_) + arenaCap_) return false;
    if (!vm::protect(reinterpret_cast<void*>(va), len, prot)) return false;
    // Update PTE protections for the covered identity range when mapped.
    const u64 pa = va - reinterpret_cast<u64>(arena_);
    for (u64 off = 0; off < len; off += 0x1000)
        cpuPt_.setProt(pa + off, prot);
    return true;
}

bool GuestMemory::queryVa(u64 va, BlockInfo* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (vaMaps_.empty()) return false;
    auto it = vaMaps_.upper_bound(va);
    if (it == vaMaps_.begin()) return false;
    --it;
    if (va >= it->first + it->second.len) return false;
    if (out) {
        out->va = it->first;
        out->len = it->second.len;
        out->prot = it->second.prot;
        out->pa = it->second.pa;
        out->name = it->second.name;
    }
    return true;
}

u64 GuestMemory::gpuReserve(u64 len, u64 alignment, u64 gpuVaFixed) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (gpuVaFixed != 0) {
        if (gpuVaFixed % 0x1000 || len % 0x1000 ||
            gpuVaFixed + len > kGpuVaSize) return 0;
        // Must be free.
        for (const auto& [gva, m] : gpuMaps_) {
            (void)m;
            if (gpuVaFixed < gva + m.len && gva < gpuVaFixed + len) return 0;
        }
        return gpuVaFixed;
    }
    const u64 va = gpuVaAlloc_.alloc(len, alignment ? alignment : 0x1000, 0, 0);
    return va;
}

bool GuestMemory::gpuMap(u64 gpuVa, u64 pa, u64 len, vm::Prot prot) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto paIt = paAllocs_.find(pa);
    if (paIt == paAllocs_.end() || paIt->second.len != len) {
        LOG_ERROR("GuestMemory: gpuMap of unknown pa=0x%llx len=%llu",
                  static_cast<unsigned long long>(pa),
                  static_cast<unsigned long long>(len));
        return false;
    }
    if (!gpuPt_.map(gpuVa, len, pa, prot)) {
        LOG_ERROR("GuestMemory: GPU page-table map failed (gpuVa=0x%llx "
                  "double-mapped?)", static_cast<unsigned long long>(gpuVa));
        return false;
    }
    ++paIt->second.mapRefs;
    gpuMaps_[gpuVa] = {len, prot, pa};
    return true;
}

bool GuestMemory::gpuUnmap(u64 gpuVa, u64 len) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = gpuMaps_.find(gpuVa);
    if (it == gpuMaps_.end() || it->second.len != len) return false;
    if (!gpuPt_.unmap(gpuVa, len)) return false;
    auto paIt = paAllocs_.find(it->second.pa);
    if (paIt != paAllocs_.end() && paIt->second.mapRefs > 0)
        --paIt->second.mapRefs;
    gpuMaps_.erase(it);
    gpuVaAlloc_.free(gpuVa, len);
    return true;
}

void* GuestMemory::gpuTranslate(u64 gpuVa) const {
    std::lock_guard<std::mutex> lk(mtx_);
    u64 pa = 0;
    if (!gpuPt_.translate(gpuVa & ~0xFFFull, &pa)) return nullptr;
    pa += gpuVa & 0xFFF;                     // intra-page offset
    if (pa >= arenaCap_) return nullptr;
    return arena_ + pa;
}

bool GuestMemory::gpuTranslateRaw(u64 gpuVa, u64* paOut) const {
    std::lock_guard<std::mutex> lk(mtx_);
    u64 pa = 0;
    if (!gpuPt_.translate(gpuVa & ~0xFFFull, &pa)) return false;
    pa += gpuVa & 0xFFF;
    if (paOut) *paOut = pa;
    return pa < arenaCap_;
}

u64 GuestMemory::bytesAllocated() const {
    std::lock_guard<std::mutex> lk(mtx_);
    u64 total = 0;
    for (u32 i = 0; i < kPoolCount; ++i) total += pools_[i].usedBytes();
    return total;
}

std::string GuestMemory::regionMap() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out;
    char line[160];
    std::snprintf(line, sizeof(line), "  %-18s %-14s %s\n",
                  "REGION", "SIZE", "RANGE");
    out += line;
    for (const auto& [pa, a] : paAllocs_) {
        std::snprintf(line, sizeof(line), "  %-18s %-11llu B  PA 0x%010llx-0x%010llx\n",
                      a.name.c_str(), static_cast<unsigned long long>(a.len),
                      static_cast<unsigned long long>(pa),
                      static_cast<unsigned long long>(pa + a.len - 1));
        out += line;
    }
    for (const auto& [va, m] : vaMaps_) {
        std::snprintf(line, sizeof(line), "  %-18s %-11llu B  VA 0x%012llx\n",
                      m.name.c_str(), static_cast<unsigned long long>(m.len),
                      static_cast<unsigned long long>(va));
        out += line;
    }
    return out;
}

std::string GuestMemory::gpuMapDump() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out;
    char line[160];
    std::snprintf(line, sizeof(line), "  %-14s %-11s %s\n",
                  "GPU VA", "SIZE", "PA");
    out += line;
    for (const auto& [gva, m] : gpuMaps_) {
        std::snprintf(line, sizeof(line), "  0x%010llx  %-11llu  0x%010llx\n",
                      static_cast<unsigned long long>(gva),
                      static_cast<unsigned long long>(m.len),
                      static_cast<unsigned long long>(m.pa));
        out += line;
    }
    return out;
}

} // namespace psb
