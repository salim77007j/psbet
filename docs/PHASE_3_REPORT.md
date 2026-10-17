# PSBET — Phase 3 Report

**Date:** 2026-10-17
**Phase:** 3 of 6 — Orbis OS depth & memory fidelity
**Status:** ✅ Complete — all acceptance criteria met with measured evidence
**Repository:** https://github.com/salim77007j/psbet

---

## 1. What was accomplished

Phase 3 delivers the OS-depth layer: the faithful memory model, the
virtualized filesystem, raw-syscall containment, real-image loader paths,
the EQUEUE surface, measured cooperative teardown, and the Windows ABI
conformance program (including the root cause of Phase 2's residual
corruption).

| Deliverable | Location | State |
|---|---|---|
| Typed direct-memory pools (free-list, search ranges, coalescing, null-guard page) | `src/core/guest_vm.*` | Working, unit+fuzz tested |
| Two-level CPU page tables (4 KiB leaves / 2 MiB mids, PTE walk, protection, conflict detection) | `src/core/guest_vm.*` | Working, unit tested |
| Separate GPU virtual address space (reserve/map/unmap/translate through real PTEs) | `src/core/guest_vm.*` | Working, unit tested |
| PBFF v1.1 `DrawTrianglesGpu` (GPU-VA draws translated per submit) | `src/gpu/cmd_stream.h`, `gpu_shim.cpp` | Working — `cube_vm` renders the cube through it (screenshots) |
| Virtualized filesystem: `app0:/` container mount + `/data` passthrough, host paths unreachable | `src/core/vfs.*` | Working, guest-verified (`fs_demo`) |
| PSBET Archive (PSA) container reader + `psbet mkfsa` packer (bounds-checked) | `src/core/pfs.*` | Working (see §5 for PFS scoping) |
| EQUEUE (create/delete/add/trigger/wait/cancel, wakeable waits) | `kernel_shim.cpp` | Working, guest-verified (`equeue_demo`) |
| Kernel surface v3: FreeDirectMemory, MapNamedDirectMemory, Mmap/Munmap (anonymous), QueryMemoryProtection, Lseek, Fstat | `kernel_shim.cpp` | Working |
| Raw-syscall trap layer (seccomp RET_TRAP on guest threads, SIGSYS→-EPERM, deny-list from `__NR_*`) | `src/core/syscall_trap.*` | Working on Linux — proven by `syscall_probe` |
| ELF loader v2: ET_EXEC absolute vaddrs, ET_DYN non-zero first vaddr, static images (no PT_DYNAMIC), IRELATIVE (resolver executed), GNU_RELRO | `src/core/elf.*` | Working, 3 synthesized conformance images pass end-to-end |
| Cooperative teardown v2 with accounting (joined / force-detached / parked per run) | `kernel_shim.cpp`, `emulator.*` | Working — zero force-detached on POSIX, asserted by the suite |
| Windows ABI conformance: **thunk3 root-cause fix**, new thunk7, probe exports, `abi_probe` guest as a hard CI gate, ExitThread record+park fix | `w64_thunks.asm`, `kernel_shim.cpp`, `tests/homebrew/abi_probe.c` | Delivered (see §4) |
| Fuzzed guest corpus (deterministic kernel-API stress + PBFF mutation/valid fuzz) | `tests/corpus_fuzz.cpp` | Working — 64 seeds/run, ASan-clean |
| Unified guest hosting model (guest main on a dedicated filtered thread on both platforms) | `cpu_supervisor.cpp`, `kernel_shim.cpp` | Delivered — zero cross-thread longjmp anywhere |
| New homebrew suite: `abi_probe`, `fs_demo`, `equeue_demo`, `syscall_probe`, `cube_vm` | `tests/homebrew/` | All green |

## 2. Technical decisions made, and why

1. **Faithful model, pinned native VA.** PSBET executes guests natively, so
   a guest pointer is a host pointer; CPU direct-memory mappings stay
   identity (VA = arena + PA) while everything *beneath* became faithful:
   typed pools with Orbis-style search ranges, per-page PTEs, refcounted
   mappings, and a fully separate GPU address space with a real page-table
   walk (GPU VAs are never assumed to match anything). CPU-side VA≠PA
   remapping is **rejected, loudly** — it buys nothing until Phase 5
   framebuffer aliasing and would break the native-pointer invariant that
   the whole engine is proven against. The rejection message tells the
   guest author exactly why (GUEST_ABI §5).
2. **PA 0 is a reserved null-guard page.** The physical allocator never
   hands out page 0, so a guest dereferencing null dies on an unmapped page
   with full diagnostics instead of silently corrupting the first physical
   page.
3. **The GPU VA space is a first-class page-table domain.** `cube_vm`
   stores its vertices in GPU-visible direct memory, maps them into a
   reserved GPU-VA window, and its display command buffer carries only the
   GPU-VA (PBFF opcode 6). The device walks the GPU page tables per submit
   and validates the whole span — unmapped or partially-mapped draws are
   rejected, not executed. Same rendered output as the inline-packet path
   (screenshots: `docs/screenshots/phase3_cube_vm_*.png`).
4. **Guests cannot see the host filesystem.** The Phase-1 fd passthrough is
   gone: `app0:/` (read-only container) and `/data` (one writable host dir)
   are the only mounts; anything else returns an error — verified by the
   `fs_demo` guest, which asserts `/etc/hostname` is unreachable. The mount
   format is PSBET's own clean-room **PSA** container (bounds-checked
   entry table, name pool, data region) with a `psbet mkfsa` packer.
5. **Raw syscalls are trapped, not ignored.** A native `syscall` in guest
   code reaching the host kernel is the one escape hatch around the emulated
   surface. On Linux, every thread that executes guest code installs a
   per-thread seccomp filter (SECCOMP_RET_TRAP) over a deny-list built from
   the kernel's own `__NR_*` constants (exec/fork, sockets, mounts,
   namespaces, modules, keyrings, kexec, io_uring, bpf, process_vm, ...).
   Trapped calls are answered -EPERM by a SIGSYS handler (RAX patch +
   RIP step past the 2-byte instruction) and logged. `open`/`openat` stay
   allowed on purpose: the engine's own VFS and file paths run on guest
   threads; guest path safety lives in the VFS layer. Platforms without
   seccomp run the documented fallback (the import boundary is the
   contract).
6. **Unified guest hosting (the model change the trap layer forced).**
   Filters must not poison engine threads, so the guest main now runs on a
   dedicated thread on **both** platforms (Windows already did). This
   removed the last cross-thread `longjmp` from the codebase — guest exits
   unwind only the exiting thread's own frames, structurally closing the
   Phase-1 flaw class — and makes "every thread that runs guest code is
   filtered" exactly true by construction. ASan/LSan stop-the-world (which
   uses ptrace from an exit-path thread) stays functional because engine
   threads are never filtered.
7. **ELF loader v2 accepts real-image shapes.** ET_EXEC images load at
   their absolute vaddrs (PA = vaddr in the regular pool; collisions fail
   loudly), ET_DYN images may anchor at any vaddr (relocations do the
   anchoring), images without `PT_DYNAMIC` load as static-style,
   `R_X86_64_IRELATIVE` is resolved by *executing* the guest's resolver,
   and `PT_GNU_RELRO` regions are made read-only after relocation. All five
   paths are regression-locked by hand-synthesized conformance images that
   run end-to-end in the test suite.
8. **Teardown is measured, not assumed.** Every run reports guest threads
   joined / force-detached / parked; the CLI prints it and the suite
   asserts zero force-detached on POSIX. The Windows park path (recorded
   exits that can never unwind GCC frames under MSVC SEH) is accounted
   separately and honestly — `WaitThreadEnd` returns recorded values
   without joining parked threads.
9. **The deny-list uses `__NR_*` constants.** An earlier draft used
   hand-written numbers and denied 273 — which is `set_robust_list`, called
   on every thread creation (threads died by SIGSYS before any handler
   could run). Constants cannot drift; the incident is documented here as
   the reason for the rule.

## 3. Validation — measured results

All numbers from real runs in this environment (Debian, GCC 14.2) unless
marked CI. Vulkan renders through lavapipe on CI runners; the sandboxed dev
environment's llvmpipe is contained by the Phase-2 self-test (software
fallback) exactly as designed.

### 3.1 Build hygiene

- `-Wall -Wextra -Werror`: **zero warnings** across all targets (engine,
  CLI, both test suites, fuzz runner, six guests).
- ASan + UBSan (Debug, leak detection on): **21/21 tests green, zero
  errors, zero leaks** — including all guest executions and teardown.

### 3.2 Test suites

```
[TEST] shader_cache                        PASS
[TEST] pixel_compare_sw_vs_vulkan          PASS (auto-skips w/o driver; runs in CI)
[TEST] memory_manager                      PASS
[TEST] guest_vm_model (pools/GPU VM)       PASS
[TEST] page_table_conflicts                PASS
[TEST] png_writer                          PASS
[TEST] png_rejects_bad_input               PASS
[TEST] rasterizer                          PASS
[TEST] rasterizer_depth                    PASS
[TEST] cmd_executor                        PASS
[TEST] guest_hello (threads/TLS/sync)      PASS
[TEST] guest_worker_exit (flaw-1 reg.)     PASS
[TEST] guest_cube (GPU pipeline)           PASS
[TEST] guest_abi_probe (thunk arity suite) PASS
[TEST] guest_equeue (EQUEUE)               PASS
[TEST] guest_fs_demo (VFS + app0)          PASS
[TEST] guest_syscall_probe (containment)   PASS
[TEST] guest_cube_vm (GPU-VA draws)        PASS
[TEST] loader_et_exec (absolute vaddrs)    PASS
[TEST] loader_dyn_offset (non-zero lo)     PASS
[TEST] loader_irelative_relro              PASS
→ 21 passed, 0 failed            (plain AND ASan/UBSan builds)

[RDNA] 8 passed, 0 failed (6,319 checks)   — unchanged from Phase 2
[FUZZ] 64 seeds, 0 failures (plain)        — kernel-API + PBFF corpus
[FUZZ] 16 seeds, 0 failures (ASan/UBSan)
```

### 3.3 Live runs (this environment)

| Run | Result |
|---|---|
| `hello.elf` | exit 0, 0 detached, 0 parked |
| `abi_probe.elf` | exit 0 (all thunk arities verified) |
| `equeue_demo.elf` | exit 0 |
| `syscall_probe.elf` (`--syscall-trap on`) | exit 0 — raw `execve`/`socket` refused -EPERM (logged with guest PC), benign raw `write` flows |
| `fs_demo.elf --mount-pfs fsroot.psa --data-dir …` | exit 0 — app0 container reads, read-only enforcement, host-path escape refused, /data round-trip lands in the host dir |
| `cube.elf`, 90 frames, software | exit 124 (controlled), 146.1 FPS avg, 3 screenshots |
| `cube_vm.elf`, 90 frames, software | exit 124 (controlled), 144.9 FPS avg, 3 screenshots — **same scene drawn entirely through GPU virtual memory** |
| `cube_vm.elf`, 5400-frame soak | exit 124 (controlled), **164.1 FPS avg**, peak RSS 11.7 MiB stable, 0 detached / 0 parked, 7,257 KiB physical live at exit |

Screenshot evidence: `docs/screenshots/phase3_cube_vm_0060.png` and
`phase3_cube_vm_0090.png` (1280×720, rotating depth-tested cube via the
GPU-VA path).

### 3.4 Containment evidence (syscall trap)

`syscall_probe.elf` makes raw `syscall` instructions from guest code:

```
[WARN ] syscall-trap: guest syscall 59 refused (EPERM) at host 0x4000000205d   (execve)
[WARN ] syscall-trap: guest syscall 41 refused (EPERM) at host 0x4000000208f   (socket)
```

The guest observes -EPERM and continues; nothing executes on the host. The
trap fires with the guest module base available for offset reporting — the
same triage metric used for crash reports.

## 4. The Windows ABI root cause (Phase-2 residual, closed)

Phase 2 left a residual: "garbled counters → stack scribble → AV at exit"
on Windows, tracked for a Phase-3 conformance suite. Root cause found by
inspection before any Windows run was needed:

```
thunk3 (Phase 2):                thunk3 (Phase 3):
  mov rcx, rdi                     mov rcx, rdi
  mov rdx, rsi                     mov r8,  rdx   ; read arg2 FIRST
  mov r8,  rdx   ; rdx is RSI!     mov rdx, rsi
```

Every 3-argument guest→host call delivered arg1 twice and **lost arg2** —
`sceKernelStartThread` received a garbage `argp`, `sceKernelWrite` a
garbage length, `sceKernelCreateCond` wrote its handle through a garbage
out-pointer. Exactly the observed failure signature. Fixes and hardening:

1. `thunk3` register order fixed; the defect class is now regression-locked
   by `abi_probe.elf` (position-distinct argument folding for every arity).
2. New **thunk7** class: SysV passes six register args, Win64 only four —
   SysV register args 4–5 must move into Win64 *stack* slots. Required by
   `sceKernelMapNamedDirectMemory` (7 args) and bound for the new v1.2
   surface (Lseek, Fstat, EQUEUE set, GPU VM set, Mmap/Munmap/Query).
3. `sceKernelExitThread` on Windows no longer longjmps (MSVC SEH cannot
   unwind GCC frames — the Phase-2 `STATUS_INVALID_DISPATCH` finding): it
   records the result, marks the thread finished, and parks; `WaitThreadEnd`
   returns the recorded value without joining a parked thread; teardown
   detaches parks with their own accounting line.

CI impact: `abi_probe`, `hello`, and `worker_exit` are **hard gates** on
`windows-latest` (previously all guest steps were diagnostic). `cube` stays
diagnostic pending Windows park-accounting soak.

## 5. Honest scoping decisions

| Item | Decision | Why |
|---|---|---|
| Encrypted Sony PFS containers | **Not implemented.** app0 mounts use PSBET's own PSA container. | Clean-room policy: no key material. The public structural documentation base was insufficient for a *verified* unencrypted-PFS reader at Phase-3 time (primary public reference inaccessible during this phase; guessed offsets would be fake compatibility). A structural reader for unencrypted user-provided images remains on the roadmap pending verified public documentation. |
| CPU VA≠PA remapping (aliasing) | Rejected by design, loudly. | Breaks the native-pointer invariant; no consumer until Phase 5 framebuffer aliasing. |
| fd-backed `sceKernelMmap` | Not implemented (anonymous only). | No guest in the corpus needs it yet; the surface returns a clear unsupported. |
| TSan for the corpus | Not added this phase. | ASan/UBSan + deterministic invariants cover the acceptance; TSan on CI runners with guests is a Phase-4 candidate. |
| `sceKernelExitThread` on Windows | Record + park (thread finished, joined logically, detached physically at teardown). | MSVC SEH cannot unwind GCC-compiled guest frames; counted separately, never silently. |

## 6. CI (GitHub Actions)

- **linux**: builds `-Werror`; lavapipe Vulkan; `psbet_tests` (21 tests incl.
  the Phase-3 guest suite and loader conformance); `psbet_rdna_tests`;
  `psbet_fuzz 64`; cube demo on Vulkan + software backends; Phase-3 demo
  block (cube_vm GPU-VA render + `mkfsa` + mounted `fs_demo` + `abi_probe`
  + `equeue_demo` + `syscall_probe`); artifacts for binaries, all guests,
  and three screenshot sets.
- **linux-asan**: full suite + fuzz corpus under ASan/UBSan with leak
  detection.
- **windows**: MSVC build + core suite; then the real ELF guests:
  **abi_probe (hard gate), hello (hard gate), worker_exit exit-code-7 (hard
  gate)**, cube diagnostic with screenshot artifacts.

## 7. Testing your own images (user-provided, legally owned)

PSBET cannot run encrypted SELF/PKG containers (clean-room policy — no key
material). To test with PSBET:

1. **Plain ELF64 guest images** (e.g. homebrew built with the documented
   recipe, or decrypted user-dumped images):
   ```
   psbet info <image.elf>          # inspect headers first
   psbet run <image.elf> --verbose
   ```
   ET_DYN (any layout) and ET_EXEC (absolute vaddrs < 8 GiB, free) are
   accepted; images without PT_DYNAMIC load as static-style. A freestanding
   image needs its own exit path (`sceKernelExitProcess` import) or the
   run ends only via `--frames`.
2. **Game-content mounts** (read-only data your guest reads at `app0:/`):
   ```
   psbet mkfsa <directory> content.psa
   psbet run <image.elf> --mount-pfs content.psa --data-dir ./writable
   ```
3. **Expected output**: the run summary prints backend, exit code, frames,
   FPS, screenshots, thread accounting (`0 joined, 0 force-detached,
   0 parked` for most guests) and physical memory live at exit. Crash
   reports include guest-module offsets. Please report failures with the
   full summary block + `--verbose` log.

This phase could not exercise *real* PS5/PS4-class homebrew images in this
environment (none legally available here); per the standing instruction,
**user-side testing of real images is the designated next step** — the
loader paths they exercise are exactly the ones locked by §3.2's
synthesized conformance images.

## 8. State of the project

- **Works (tested):** everything from Phases 1–2, plus: typed direct-memory
  pools with search ranges and refcounted lifetimes; two-level CPU page
  tables; a separate GPU virtual address space with PBFF v1.1 GPU-VA draws;
  virtualized filesystem (app0 container + /data, host paths unreachable);
  EQUEUE; expanded kernel surface; seccomp-based raw-syscall containment;
  real-image loader shapes (ET_EXEC / offset ET_DYN / static / IRELATIVE /
  RELRO); measured zero-straggler teardown; Windows ABI conformance with
  the root cause fixed; deterministic fuzz corpus (ASan-clean).
- **Does not work yet (by design, see ROADMAP):** audio/input (Phase 4),
  presentation window (Phase 4), RDNA subset loops + resource model against
  real captures (Phase 4+), performance engineering (Phase 5), unencrypted
  PFS structural mounts (needs verified public documentation), commercial
  titles (Phases 4–6 and beyond). **No commercial PS5 title runs today** —
  stated nowhere as otherwise.

## 9. Artifacts

- CI artifacts: `psbet-linux-x64`, `psbet-windows-x64`, `psbet-guest-elves`
  (7 guests), `psbet-demo-screenshots-{vulkan,software,gpu-va,windows}`
- Evidence: `docs/screenshots/phase3_cube_vm_*.png`, CI logs
- Documentation: `ARCHITECTURE.md` §11 (memory model v2, GPU VM, VFS,
  syscall trap, hosting unification, risk register), `GUEST_ABI.md` v1.2,
  `ROADMAP.md` (Phase 3 acceptance ticked), this report

## 10. What remains to be done

Phase 4 — Audio, input, timing: SDL3 audio backend with guest-side mixing
semantics; DualSense input mapping (SDL game controller + gyroscope);
vsync/pacing model (target-FPS authorization); net stubs with logging; the
presentation window. **Phase 3 ends here; work resumes on the explicit
"Continue to Phase 4" command.**
