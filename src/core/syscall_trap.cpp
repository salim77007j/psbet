// ============================================================================
//  PSBET — syscall_trap.cpp
//  seccomp-based raw-syscall trap for guest threads (Linux x86-64).
//
//  Design notes:
//   * seccomp filters are PER-THREAD on Linux — installed at the top of each
//     guest thread trampoline, they never affect engine or host threads.
//     Threads created BY a filtered guest thread inherit the filter, which is
//     exactly the desired containment (guest-spawned guests are trapped too).
//   * The deny-list covers host-escaping surface (process/exec, sockets,
//     mounts/namespaces, modules, keyrings, kexec, io_uring, ...). Everything
//     the PSBET engine uses on guest threads (futex, write, clock_nanosleep,
//     mmap, brk, rt_sig*, sched_yield, getrandom, ...) stays allowed, so
//     engine internals running on guest threads keep working.
//   * A trapped call raises SIGSYS with si_code == SYS_SECCOMP. The handler
//     writes -EPERM into RAX, steps RIP past the 2-byte `syscall` instruction
//     when the context still points at it, and returns — guest execution
//     continues with an errno-style failure, like any refused syscall.
//   * No emulation recursion: the handler never performs guest-triggered I/O
//     beyond the engine's own logging path.
// ============================================================================
#include "syscall_trap.h"

#include "core/log.h"

#if !defined(_WIN32) && defined(__x86_64__) && defined(__linux__)

#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <vector>
#include <ucontext.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace psb::syscall_trap {

namespace {

// Deny-list of host-escaping syscalls, expressed with the kernel's own
// __NR_* constants (hand-written numbers drift; constants cannot). Notes:
//   * open/openat stay ALLOWED — the engine's own VFS/file paths run on
//     guest threads; guest path safety lives in the VFS layer.
//   * clone/clone3 stay ALLOWED — guest threads are host threads.
//   * Everything here refuses with SIGSYS -> -EPERM (logged).
constexpr u32 kDenied[] = {
    __NR_fork, __NR_vfork, __NR_execve, __NR_execveat,
    __NR_socket, __NR_connect, __NR_accept, __NR_accept4,
    __NR_bind, __NR_listen, __NR_socketpair,
    __NR_ptrace,
    __NR_mount, __NR_umount2, __NR_chroot, __NR_mknod, __NR_mknodat,
    __NR_open_tree, __NR_fsopen, __NR_fsmount, __NR_move_mount,
    __NR_mount_setattr,
    __NR_fanotify_init, __NR_fanotify_mark,
    __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
    __NR_kexec_load, __NR_kexec_file_load,
    __NR_reboot, __NR_swapon, __NR_swapoff,
    __NR_setns, __NR_unshare,
    __NR_init_module, __NR_delete_module, __NR_finit_module,
    __NR_add_key, __NR_request_key, __NR_keyctl,
    __NR_bpf,
    __NR_pidfd_send_signal,
    __NR_process_vm_readv, __NR_process_vm_writev,
    __NR_memfd_secret,
};

thread_local bool g_installed = false;

void sigsysHandler(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    ucontext_t* uc = static_cast<ucontext_t*>(ctx);
    const int nr = info ? info->si_syscall : -1;
    const u64 callAddr = info ? reinterpret_cast<u64>(info->si_call_addr) : 0;

    LOG_WARN("syscall-trap: guest syscall %d refused (EPERM) at host 0x%llx",
             nr, static_cast<unsigned long long>(callAddr));

    if (uc) {
        // Result register: errno-style refusal.
        uc->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(-EPERM);
        // The kernel normally resumes AFTER the syscall instruction. If the
        // context still points AT it (0F 05), step past it explicitly.
        greg_t rip = uc->uc_mcontext.gregs[REG_RIP];
        const u8 op0 = *reinterpret_cast<const u8*>(static_cast<u64>(rip));
        const u8 op1 = *reinterpret_cast<const u8*>(static_cast<u64>(rip) + 1);
        if (op0 == 0x0F && op1 == 0x05) rip += 2;
        uc->uc_mcontext.gregs[REG_RIP] = rip;
    }
}

// BPF program: load the syscall nr, TRAP on deny-list match, ALLOW otherwise.
std::vector<sock_filter> buildFilter() {
    std::vector<sock_filter> prog;
    prog.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                            offsetof(struct seccomp_data, nr)));
    for (u32 nr : kDenied) {
        prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP));
    }
    prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    return prog;
}

} // namespace

bool available() { return true; }

bool installOnCurrentThread(Mode mode) {
    if (mode == Mode::Off) return true;
    if (g_installed) return true;

    struct sigaction sa{};
    sa.sa_sigaction = sigsysHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, nullptr) != 0) {
        LOG_WARN("syscall-trap: sigaction(SIGSYS) failed — import-boundary fallback");
        return mode != Mode::Required;
    }

    static const std::vector<sock_filter> prog = buildFilter();
    struct sock_fprog fprog;
    fprog.len = static_cast<unsigned short>(prog.size());
    fprog.filter = const_cast<struct sock_filter*>(prog.data());

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_WARN("syscall-trap: PR_SET_NO_NEW_PRIVS failed (errno %d) — fallback", errno);
        return mode != Mode::Required;
    }
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
        LOG_WARN("syscall-trap: seccomp filter refused (errno %d) — "
                 "running with the import-boundary fallback", errno);
        return mode != Mode::Required;
    }
    g_installed = true;
    LOG_DEBUG("syscall-trap: filter active on this guest thread (%zu deny entries)",
              sizeof(kDenied) / sizeof(kDenied[0]));
    return true;
}

const char* describe(Mode mode) {
    switch (mode) {
        case Mode::Off:      return "off (import boundary only)";
        case Mode::Required: return "required";
        case Mode::On:       return "on (seccomp trap)";
    }
    return "?";
}

#else // non-Linux / non-x86-64

bool available() { return false; }

bool installOnCurrentThread(Mode mode) {
    if (mode == Mode::Required) {
        LOG_ERROR("syscall-trap: not available on this platform");
        return false;
    }
    return true;
}

const char* describe(Mode mode) {
    switch (mode) {
        case Mode::Off:      return "off";
        case Mode::Required: return "required (unavailable here)";
        default:             return "unavailable — import boundary fallback";
    }
}

#endif

} // namespace psb::syscall_trap
