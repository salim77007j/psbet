// ============================================================================
//  PSBET — memory.cpp
//  Legacy facade over the Phase-3 GuestMemory model.
// ============================================================================
#include "memory.h"

#include "core/log.h"

namespace psb {

u64 MemoryManager::allocate(size_t size, size_t alignment, const char* name,
                            vm::Prot prot) {
    if (size == 0) return 0;
    const u64 pa = mem_.allocDirect(GuestMemory::kPoolRegular, size,
                                    alignment ? alignment : 1, 0, 0, name);
    if (!pa) return 0;
    const u64 va = mem_.mapDirect(pa, size, prot, name);
    if (!va) {
        mem_.freeDirect(pa, size);
        return 0;
    }
    LOG_DEBUG("MemoryManager: allocate('%s', %zu B) -> pa 0x%llx va 0x%llx",
              name ? name : "unnamed", size,
              static_cast<unsigned long long>(pa),
              static_cast<unsigned long long>(va));
    return va;
}
} // namespace psb
