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

## Phase 2 — Vulkan GPU backend & shader recompiler

Scope: Vulkan device backend behind the existing PBFF surface; RDNA ISA
disassembler (standalone, table-driven); shader recompiler pipeline
(RDNA → SPIR-V) for vertex/fragment first; pipeline & shader caches; depth/
blend state translation; the Windows guest-hosting shim (TLS base switching
+ exception mapping) so ELF guests run on Windows too.

**Acceptance targets:**
- Cube demo renders identically through Vulkan and software backends
  (pixel-compare harness)
- Frame time for a 100k-triangle scene < 8 ms on a mid-range dGPU
- Windows runner executes hello/cube guests
- Recompiler unit tests on >= 500 captured shader cases

## Phase 3 — Orbis OS depth & memory fidelity

Scope: faithful page-table emulation (typed direct-memory pools, GPU
virtual memory), PFS read-only mounting for user-provided images, expanded
libkernel/libc surface (file I/O, event queues, EQUEUE, more of
`sceKernel*`), cooperative guest-thread teardown, raw-syscall trap layer
(seccomp-based) with full emulation fallback, ELF loader support for real
user-dumped decrypted images.

**Acceptance targets:**
- Real homebrew PS5/PS4-class images (user-provided, legally owned) load and
  reach first frame
- Zero silent memory-ordering bugs under a fuzzed guest corpus
- Process teardown: zero detached threads, zero leaks under ASan

## Phase 4 — Audio, input, timing

Scope: SDL3 audio backend with guest-side mixing semantics; DualSense input
mapping (SDL game controller + gyroscope); vsync/pacing model (target FPS
authorization like console boost modes); net stubs with logging.

**Acceptance targets:**
- Audio: glitch-free 48 kHz stereo under load; measured latency < 40 ms
- Input: end-to-end latency < 25 ms at 60 FPS pacing
- Guest timing APIs monotonic and drift-free over 24 h simulated soak

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
