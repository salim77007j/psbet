// ============================================================================
//  PSBET — vm.cpp
//  Virtual-memory primitives. Linux: mmap/mprotect/munmap.
//  Windows: VirtualAlloc/VirtualProtect/VirtualFree.
// ============================================================================
#include "vm.h"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace psb::vm {

size_t pageSize() {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

void* reserve(size_t size, void* hint) {
#if defined(_WIN32)
    (void)hint; // Windows picks its own base; guest images are relocated anyway.
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    return p;
#else
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS
                    | (hint ? static_cast<int>(MAP_FIXED_NOREPLACE) : 0);
    void* p = mmap(hint, size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) {
        if (hint) {
            // Retry without a fixed address so a busy hint never aborts us.
            p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) return nullptr;
            return p;
        }
        return nullptr;
    }
    return p;
#endif
}

bool commit(void* addr, size_t size, Prot prot) {
    if (!addr || size == 0) return false;

    // Callers may pass page-unaligned ranges (ELF segments are only
    // page-*congruent*); round down/up to page granularity here.
    const size_t ps = pageSize();
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t aStart = a & ~(static_cast<uintptr_t>(ps) - 1);
    const uintptr_t aEnd   = (a + size + ps - 1) & ~(static_cast<uintptr_t>(ps) - 1);
    void* p = reinterpret_cast<void*>(aStart);
    size = static_cast<size_t>(aEnd - aStart);

#if defined(_WIN32)
    DWORD fl = PAGE_NOACCESS;
    switch (prot) {
        case Prot::NoAccess:     fl = PAGE_NOACCESS; break;
        case Prot::ReadOnly:     fl = PAGE_READONLY; break;
        case Prot::Write:
        case Prot::ReadWrite:    fl = PAGE_READWRITE; break;
        case Prot::Exec:         fl = PAGE_EXECUTE; break;
        case Prot::ReadExec:     fl = PAGE_EXECUTE_READ; break;
        case Prot::ReadWriteExec:fl = PAGE_EXECUTE_READWRITE; break;
    }
    return VirtualAlloc(p, size, MEM_COMMIT, fl) != nullptr;
#else
    int pr = PROT_NONE;
    if ((static_cast<u32>(prot) & static_cast<u32>(Prot::ReadOnly))) pr |= PROT_READ;
    if ((static_cast<u32>(prot) & static_cast<u32>(Prot::Write)))    pr |= PROT_WRITE;
    if ((static_cast<u32>(prot) & static_cast<u32>(Prot::Exec)))     pr |= PROT_EXEC;
    return mprotect(p, size, pr) == 0;
#endif
}

bool protect(void* addr, size_t size, Prot prot) {
    return commit(addr, size, prot); // mprotect == VirtualProtect semantics here
}

bool release(void* addr, size_t size) {
#if defined(_WIN32)
    (void)size;
    return VirtualFree(addr, 0, MEM_RELEASE) != 0;
#else
    return munmap(addr, size) == 0;
#endif
}

} // namespace psb::vm
