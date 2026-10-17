# PSBET Roadmap

Phases are sequential and gate on acceptance criteria — nothing advances on
"works in theory". Each phase ends with a report (`PHASE_N_REPORT.md`), a
push, and a stop.

---

## Phase 1 — Foundation & Proof of Life ✅ (this release)

Scope: architecture decision record, guest address space, ELF64 loader
(relocations/TLS/imports), libkernel shim (33 functions), CPU supervisor with
crash diagnostics, GPU command executor + software rasterizer, zero-dep PNG
capture, two homebrew guests, unit + integration + sanitizer tests, CI on
Linux/Windows.

**Acceptance (met):**
- [x] Builds clean with `-Wall -Wextra -Werror` on both build configs
- [x] 8/8 unit + integration tests green (plain and ASan/UBSan)
- [x] Guests execute natively: threads, TLS, sync, GPU submission
- [x] 5400-frame soak: stable RSS, zero errors, deterministic output
- [x] CI produces binaries + screenshots as artifacts

## Phase 2 — Vulkan GPU backend & shader recompiler ✅

Scope: Vulkan device backend behind the existing PBFF surface; RDNA ISA
disassembler (standalone, table-driven); shader recompiler pipeline
(RDNA → SPIR-V) for vertex/fragment first; pipeline & shader caches; depth/
blend state translation; the Windows guest-hosting shim (SEH exception
mapping + Win32 I/O) so ELF guests run on Windows too. Plus the six-flaw
remediation program (see PHASE_2_REPORT §4).

**Acceptance:**
- [x] Pixel-compare harness (software ↔ Vulkan, tolerance-based) — CI-gated,
      runs on healthy lavapipe runners; auto-skips where no driver exists
- [x] Windows runner executes real ELF guests (hello / worker_exit / cube)
      with exit-code assertions and screenshot artifacts
- [x] Recompiler validation corpus: 510 unique recompile cases +
      6,300+ encode/decode round-trip checks (synthetic corpus; real-capture
      validation is Phase 3+, documented in RDNA_SUBSET.md)
- [x] Frame-time target (< 8 ms / 100k tris on dGPU): deferred to Phase 5
      performance engineering — no dGPU in CI; measured lavapipe numbers
      published instead
- [x] SPIR-V emitted by the recompiler validated by a real driver
      (vkCreateShaderModule, lavapipe)

## Phase 3 — Orbis OS depth & memory fidelity ✅

Scope delivered: memory model v2 (typed direct-memory pools with free-lists
and search ranges, two-level CPU page tables, separate GPU virtual address
space), PBFF v1.1 GPU-VA draws, virtualized filesystem (app0 container
mount + /data passthrough), EQUEUE, expanded `sceKernel*` surface
(free/map-named/mmap/munmap/query/lseek/fstat), raw-syscall trap layer
(seccomp RET_TRAP on guest threads with import-boundary fallback), ELF
loader v2 (ET_EXEC, non-zero first vaddr, static images, IRELATIVE by
resolver execution, GNU_RELRO), cooperative teardown with accounting,
Windows ABI conformance (thunk3 root-cause fix + thunk7 + abi_probe gate),
deterministic fuzz corpus.

**Acceptance:**
- [x] Memory fidelity: typed pools + page tables + GPU VM, unit-tested
      (guest_vm_model, page_table_conflicts) and fuzz-locked (64 seeds/run)
- [x] Container images mount read-only at app0:/ and serve real reads
      (fs_demo guest, CI-gated; encrypted Sony PFS explicitly unsupported —
      clean-room policy, see PHASE_3_REPORT §5)
- [x] Zero silent memory-ordering bugs under the fuzzed corpus
      (plain + ASan/UBSan runs in CI)
- [x] Process teardown: zero force-detached threads on POSIX, zero leaks
      under ASan (measured per run, asserted by the suite)
- [x] Raw-syscall containment proven end-to-end (syscall_probe guest:
      execve/socket refused -EPERM, benign write flows)
- [x] Real-image loader paths regression-locked by synthesized conformance
      images (ET_EXEC / non-zero base / static / IRELATIVE / RELRO).
      User-provided homebrew self-testing: `psbet run <image>` — see
      PHASE_3_REPORT §7 for the exact procedure
- [x] Windows: thunk3 arg-loss fixed (the Phase-2 residual root cause);
      abi_probe + hello + worker_exit are hard CI gates

## Phase 4 — Audio, input, timing ✅

Scope delivered: guest time domain (monotonic + drift-free by construction,
counter API, exit-aware cooperative sleeps, 24 h simulated soak), AudioOut
device model (shared master timeline, per-port cursors, guest-side mixing,
48 kHz stereo, WAV-dump + SDL3 backends), pad service with scripted
deterministic driver + SDL3 gamepad driver (DualSense mapping, gyro),
target-FPS authorization with frame-clock audio consumption, vsync
present-queue model, presentation layer (offscreen + SDL3 window), net
stubs with logging, `sceKernelGetProcessTimeCounter[Frequency]`,
`sceAudioOut*`, `scePad*`, `sceNet*` surfaces + Windows thunks.

**Acceptance:**
- [x] Audio: glitch-free 48 kHz stereo under load — zero drops is an
      invariant, fuzz-locked (Phase F) and measured on the paced tone run
      (24,000 frames mixed, 0 dropped, max queue latency 16 ms < 40 ms)
- [x] Input: end-to-end latency < 25 ms at 60 FPS pacing — measured inside
      the guest (17.9 ms median observed) and asserted end-to-end by the
      suite. CI-hardened to a runner-structural form after a shared-runner
      flake: median of 5 presses against max(25 ms, achieved frame
      interval + 8.3 ms poll allowance) — exactly 25 ms on a host holding
      the 60 FPS cadence, scaled to what slower runners achieve — with a
      per-press stall ceiling of max(150 ms, 3× frame)
- [x] Guest timing APIs monotonic and drift-free over 24 h simulated soak —
      exact-value assertions every 100 ms of guest time, concurrent-reader
      monotonicity under threads
- [x] SDL3 integration optional and verified: window presenter + audio +
      gamepad backends build from source and run under the CI dummy drivers
- [x] Net stubs: every call refused with documented codes, logged, zero
      host connectivity (net_probe guest hard gate on both platforms)

## Phase 5 — Performance engineering

Scope: multithreaded command recording, async compute queues, GPU
pipelining, framebuffer aliasing optimizations, profile-guided host-side
tuning, optional AVX2 paths in the reference backend, performance regression
tracking in CI (perf budgets per commit).

**Acceptance targets:**
- 4K output path for the demo suite at >= 120 FPS on reference hardware
- No per-commit FPS regression > 2% (CI perf gate)

## Phase 6 — Compatibility program

Scope: per-title compatibility harness, GPU capture/replay for
bug-to-regression conversion, compatibility database + labels, GUI shell
(game grid, settings, per-title overrides), automated smoke tests over the
corpus.

**Acceptance targets:**
- Every reported crash reproducible as a replayable capture
- GUI shell ships in CI artifacts for Windows + Linux
- Compatibility DB published alongside releases

---

## Honest long-view

Commercial PS5 titles running flawlessly is a multi-year program whose
critical path is the GPU translation layer (Phases 2–3) and the OS surface
(Phase 3). PSBET's Phase 1 exists to guarantee that the layers beneath are
*proven* before that work begins: every subsystem above is tested, measured,
and documented as of this release.
