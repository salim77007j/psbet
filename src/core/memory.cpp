// ============================================================================
//  PSBET — memory.cpp
// ============================================================================
#include "memory.h"

#include <algorithm>
#include <cstdio>
#include "core/log.h"

namespace psb {

MemoryManager::~MemoryManager() {
    if (arena_) vm::release(arena_, arenaCap_);
}

bool MemoryManager::init() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (arena_) return true;

    void* p = vm::reserve(arenaCap_, reinterpret_cast<void*>(kArenaHint));
    if (!p) {
        // Fall back to any address the OS likes.
        p = vm::reserve(arenaCap_, nullptr);
        if (!p) {
            LOG_ERROR("MemoryManager: failed to reserve %llu bytes of guest arena",
                      static_cast<unsigned long long>(arenaCap_));
            return false;
        }
    }
    arena_ = static_cast<u8*>(p);
    bump_  = 0;
    LOG_INFO("MemoryManager: guest arena reserved at 0x%llx (%llu MiB)",
             static_cast<unsigned long long>(reinterpret_cast<u64>(arena_)),
             static_cast<unsigned long long>(arenaCap_ / MiB));
    return true;
}

u64 MemoryManager::allocate(size_t size, size_t alignment, const char* name,
                            vm::Prot prot) {
    if (!arena_ || size == 0) return 0;
    if (alignment == 0) alignment = 1;

    std::lock_guard<std::mutex> lk(mtx_);

    const u64 base   = reinterpret_cast<u64>(arena_);
    const u64 start  = alignUp(base + bump_, alignment);
    const u64 offset = start - base;

    if (offset + size > arenaCap_) {
        LOG_ERROR("MemoryManager: arena exhausted (need %zu bytes at offset %llu)",
                  size, static_cast<unsigned long long>(offset));
        return 0;
    }

    // Commit up-front with the requested protection. Pages stay reserved even
    // if only partially used (internal fragmentation accepted in Phase 1).
    if (!vm::commit(reinterpret_cast<void*>(start), size, prot)) {
        LOG_ERROR("MemoryManager: commit failed for region '%s'", name);
        return 0;
    }

    bump_ = offset + size;
    regions_.push_back({start, size, name ? name : "unnamed"});
    return start;
}

bool MemoryManager::protectRange(u64 addr, size_t size, vm::Prot prot) {
    if (!arena_ || size == 0) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    const u64 base = reinterpret_cast<u64>(arena_);
    if (addr < base || addr + size > base + arenaCap_) return false;
    return vm::protect(reinterpret_cast<void*>(addr), size, prot);
}

u64 MemoryManager::bytesAllocated() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return bump_;
}

std::string MemoryManager::regionMap() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out;
    char line[128];
    std::snprintf(line, sizeof(line), "  %-18s %-14s %s\n", "REGION", "SIZE", "RANGE");
    out += line;
    for (const auto& r : regions_) {
        std::snprintf(line, sizeof(line), "  %-18s %-11llu B  0x%012llx-0x%012llx\n",
                      r.name.c_str(), static_cast<unsigned long long>(r.size),
                      static_cast<unsigned long long>(r.base),
                      static_cast<unsigned long long>(r.base + r.size - 1));
        out += line;
    }
    return out;
}

} // namespace psb
