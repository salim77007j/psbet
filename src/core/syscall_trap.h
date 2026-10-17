// ============================================================================
//  PSBET — syscall_trap.h
//  Raw-syscall trap layer (Phase 3).
//
//  Guests run natively on the host CPU, so a guest that executes a raw
//  `syscall` instruction would reach the HOST kernel directly — the one
//  escape hatch around PSBET's emulated kernel surface. This layer closes it:
//
//  Linux: every guest thread installs a seccomp filter (SECCOMP_RET_TRAP)
//  for a deny-list of host-escaping syscalls (exec, fork, sockets, mounts,
//  modules, keyrings, io_uring, ...). A trapped call never executes; the
//  kernel delivers SIGSYS and PSBET's handler answers it with -EPERM (after
//  logging) and resumes the guest past the instruction. Benign syscalls the
//  engine itself needs on guest threads (futex, write, nanosleep, mmap, ...)
//  are allowed through untouched.
//
//  Other platforms: no portable equivalent exists without a kernel driver;
//  available() reports false and PSBET runs with the documented fallback —
//  the emulated import surface is the boundary (see GUEST_ABI §11).
// ============================================================================
#pragma once

#include "psbet/common.h"

namespace psb::syscall_trap {

// True when this platform can trap (Linux with libseccomp-free BPF filter).
bool available();

// Mode selected by the CLI / tests.
enum class Mode {
    Off = 0,     // no filter (documented fallback boundary)
    On,          // install on every guest thread (default on Linux)
    Required,    // refuse to run guests when the filter cannot install
};

// Install the filter on the CALLING thread. Called at the top of every guest
// thread trampoline (and before the supervisor enters guest code).
// Returns false when installation failed (only possible in Mode::Required).
bool installOnCurrentThread(Mode mode);

// Human-readable summary for logs / crash reports.
const char* describe(Mode mode);

} // namespace psb::syscall_trap
