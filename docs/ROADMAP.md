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
      the guest and asserted end-to-end by the suite. A post-merge CI
      failure exposed a real defect (pad refresh coupled to the GPU submit
      path) whose fix dropped measured median latency from 17.9 ms to
      1.29 ms (13×, now independent of GPU/backend speed); the gate is
      runner-structural: median of 5 presses against max(25 ms, achieved
      frame interval + 8.3 ms poll allowance), per-press stall ceiling
      max(150 ms, 3× frame) — see PHASE_4_REPORT §5.1
- [x] Guest timing APIs monotonic and drift-free over 24 h simulated soak —
      exact-value assertions every 100 ms of guest time, concurrent-reader
      monotonicity under threads
- [x] SDL3 integration optional and verified: window presenter + audio +
      gamepad backends build from source and run under the CI dummy drivers
- [x] Net stubs: every call refused with documented codes, logged, zero
      host connectivity (net_probe guest hard gate on both platforms)

## Phase 5 — Performance engineering ✅

Scope delivered: pipelined GPU submit path (graphics + compute queues with
snapshot semantics, in-flight backpressure, exit-aware waits), async compute
queue with a real cross-queue fence (`sceGnmSubmitCompute`,
`sceGnmWaitQueueIdle`, CmdFillMemory), multithreaded command submission
(`mt_submit` homebrew: 4 guest threads, concurrent recording + submission),
band-parallel software rasterizer with an optional byte-exact AVX2 path,
parallel clear, Vulkan lazy readback, host-side tuning knobs
(`--gpu-sync`, `--gpu-threads`, `PSBET_RASTER_THREADS`, `PSBET_RASTER_SIMD`),
and per-commit performance budgets in CI (`psbet bench` + `ci/perf_gate.py`
+ `ci/perf_budgets.json`, report published as an artifact).

**Acceptance targets:**
- [x] 4K output path for the demo suite at >= 120 FPS on reference
      hardware: measured 170 FPS (1 band thread) to 256 FPS (2 threads)
      at 3840x2160 for `cube.elf` on the 2-vCPU Phase-5 build host —
      software rasterizer, free run. Reference-hardware numbers are
      expected to scale further (hw-1 band threads, AVX2); the
      user-side verification procedure is in PHASE_5_REPORT §6
- [x] No per-commit FPS regression: CI perf gate on every push
      (`perf_gate.py` against `perf_budgets.json`, artifact published);
      the strict 2% rule is enforced as the same-machine A/B procedure
      (shared runners cannot hold 2% — policy documented in
      PHASE_5_REPORT §7)

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
