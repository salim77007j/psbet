// ============================================================================
//  PSBET — vm.h
//  Platform virtual-memory primitives (Linux mmap / Windows VirtualAlloc).
//  These are the only OS calls the whole engine depends on for memory.
// ============================================================================
#pragma once

#include <cstddef>
#include "psbet/common.h"

namespace psb::vm {

enum class Prot : u32 {
    NoAccess = 0,
    ReadOnly = 1 << 0,
    Write    = 1 << 1,   // implied with Read on most platforms
    ReadWrite = ReadOnly | Write,
    Exec      = 1 << 2,
    ReadExec  = ReadOnly | Exec,
    ReadWriteExec = ReadOnly | Write | Exec,
};

// Reserve an address-space range (PROT_NONE / MEM_RESERVE). Returns nullptr on
// failure. `hint` may be nullptr. `size` is rounded up to page size by the OS.
void* reserve(size_t size, void* hint = nullptr);

// Commit a previously reserved range with the given protection.
bool commit(void* addr, size_t size, Prot prot);

// Change protection of committed pages.
bool protect(void* addr, size_t size, Prot prot);

// Release a reservation (whole range originally reserved).
bool release(void* addr, size_t size);

// System page granularity (bytes).
size_t pageSize();

} // namespace psb::vm
