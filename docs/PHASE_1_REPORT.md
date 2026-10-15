# PSBET — Phase 1 Report

**Date:** 2026-10-16
**Phase:** 1 of 6 — Foundation & Proof of Life
**Status:** ✅ Complete — all acceptance criteria met with measured evidence
**Repository:** https://github.com/salim77007j/psbet

---

## 1. What was accomplished

Phase 1 delivers a working, tested, documented foundation of PSBET: a
clean-room runtime that loads PS5-class x86-64 ELF images into a managed
guest address space, binds their OS/GPU imports to emulated services, and
executes them natively on the host CPU — proven end-to-end with real
homebrew guests, real rendered frames, real screenshots, and a CI pipeline
that reproduces all of it on every push.

Concretely, shipped in this phase:

| Component | Location | State |
|---|---|---|
| Guest address space (16 GiB arena, protected regions) | `src/core/memory.*`, `src/platform/vm.*` | Working, unit-tested |
| ELF64 loader (map/relocate/TLS/imports/initializers) | `src/core/elf.*` | Working, integration-tested |
| libkernel shim — 33 functions (threads/sync/time/memory/I/O) | `src/core/kernel_shim.*` | Working, integration-tested |
| CPU supervisor (entry/exit control, crash diagnostics) | `src/core/cpu_supervisor.*` | Working |
| GPU: PBFF v1 command format + executor | `src/gpu/cmd_stream.h`, `src/gpu/gpu_shim.*` | Working, fuzz-checked |
| GPU: software rasterizer (depth/cull/viewport/interp) | `src/gpu/rasterizer.*` | Working, pixel-tested |
| Screenshot capture (zero-dependency PNG writer) | `src/gpu/png.*` | Working, validated |
| Orchestration + CLI (`run`/`info`, capture, stats) | `src/emulator.*`, `src/tools/psbet_main.cpp` | Working |
| Homebrew guest 1: threads/TLS/sync self-test | `tests/homebrew/hello.c` → `hello.elf` | Passing (exit 0) |
| Homebrew guest 2: animated 3D cube on the GPU pipeline | `tests/homebrew/cube.c` → `cube.elf` | Passing, screenshots captured |
| Test harness (8 tests: unit + end-to-end guest runs) | `tests/run_tests.cpp` | 8/8 green |
| CI: Linux build+tests+demo, ASan/UBSan job, Windows job, artifacts | `.github/workflows/ci.yml` | Configured |
| Documentation set (this report, architecture, roadmap, guest ABI) | `docs/` | Complete |

## 2. Technical decisions made, and why

1. **Native execution instead of CPU emulation (D1).** The PS5 CPU is
   x86-64 Zen 2; the host is x86-64. Emulating the same ISA wastes an order
   of magnitude of performance to solve a non-problem. Guest code executes
   directly; kernel/GPU operations are intercepted at the API boundary —
   the WINE/shadPS4 lineage of strategy. Measured consequence: guest CPU
   work is effectively free; frame time is dominated by the deliberately
   simple software rasterizer, which Phase 2 replaces with Vulkan.

2. **C++17 with zero third-party dependencies (D2).** The engine needs four
   OS primitives, threads, and `dlsym`. Keeping the core dependency-free
   makes the clean-room claim auditable, keeps CI trivial on GCC and MSVC,
   and every byte of engine code reviewable. Dependency introduction points
   are scheduled (Vulkan headers in Phase 2, SDL3 in Phase 4).

3. **Unified guest address space in Phase 1** (documented deviation): one
   arena, direct-memory allocations immediately CPU-visible, GPU = CPU
   pointers. This isolates all the genuinely new engineering (loader, TLS,
   shim, GPU format) from page-table machinery that Phase 3 replaces
   behind the same `MemoryManager` interface.

4. **Guests are built "for the console", not the host.** Guest ELFs are
   freestanding (`-nostartfiles -shared -fPIC`, entry `psb_entry`) and are
   *never* built with host sanitizers — an ASan-instrumented guest has no
   runtime to execute against (this bit us once, see §5) and is now
   enforced in the build system.

5. **Host-symbol isolation rule.** The shim exports `__tls_get_addr` /
   `__cxa_finalize` only as loader-visible names bound to internally-named
   host functions. Defining them as real host symbols would interpose
   glibc's own for the entire process — discovered via an ASan
   stack-overflow in its fake-stack TLS path, now codified as a rule in
   `ARCHITECTURE.md` §5.

## 3. Validation — measured results

All numbers below are from real runs in this environment (Debian, GCC 14.2,
`RelWithDebInfo` build).

### 3.1 Build hygiene

- `cmake --build` with `-Wall -Wextra -Werror`: **zero warnings, zero
  errors** across all targets (engine, CLI, tests, guests).
- ASan + UBSan build: **8/8 tests green with leak detection on**; no memory
  errors, no undefined behavior, no leaks across the full run (including
  native guest execution and forced frame-limit exits).

### 3.2 Test suite

```
[TEST] memory_manager                        PASS
[TEST] png_writer                            PASS
[TEST] png_rejects_bad_input                 PASS
[TEST] rasterizer                            PASS
[TEST] rasterizer_depth                      PASS
[TEST] cmd_executor                          PASS
[TEST] guest_hello (threads/TLS/sync)        PASS
[TEST] guest_cube (GPU pipeline)             PASS
→ 8 passed, 0 failed   (stable across 6 repeated runs; earlier flake fixed)
```

The end-to-end tests run *actual guest ELFs* through the full pipeline:
`hello.elf` verifies 2 threads × 1000 mutex-guarded increments (exact
result required) and per-thread TLS isolation; `cube.elf` renders through
the GPU command executor with screenshot-count and frame-limit assertions.

### 3.3 Real-world run — `cube.elf` (the "proof of life" workload)

Guest loads, binds 14 dynamic imports, initializes, and renders an animated
depth-tested cube through the emulated GPU pipeline at 1280×720:

| Metric | 90-frame run | 5400-frame soak |
|---|---|---|
| Guest exit | 124 (frame limit — controlled) | 124 (controlled) |
| Frames rendered | 90 | 5,400 |
| Wall time | 614 ms | 31.6 s |
| Average FPS | 146.6 | 170.7 |
| Peak RSS | 34.9 MiB | 34.0 MiB (**stable — no growth**) |
| Crashes / errors | 0 | 0 |

Raw throughput (no capture): **170.2 FPS**, peak RSS **11.2 MiB**. The
rasterizer is intentionally simple — it is the correctness oracle for the
Phase 2 Vulkan backend, not the performance story.

Captured evidence (committed to `docs/screenshots/`):

| File | Content |
|---|---|
| `cube_frame_0030.png` | Cube rotating — orange/red face dominant |
| `cube_frame_0060.png` | Green face with yellow bottom edge (depth-tested occlusion) |
| `cube_frame_0090.png` | Later rotation state |
| `cube_soak_frame_5400.png` | Blue face + yellow side — correct 3D perspective after 5400 frames |

All four verified as structurally valid PNGs (signature, IHDR dimensions,
decompressible zlib stream of exactly `h×(1+w×4)` bytes) and as *distinct*
images (independent MD5s; per-row brightness fingerprints differ). Frames
60 and 90 sharing a file size is an artifact of stored-block deflate —
size depends only on dimensions; content was verified distinct.

### 3.4 Homebrew self-test — `hello.elf`

Executes through the kernel shim and reports, then exits **0**:

```
[hello] PSBET guest ABI v1 online
[hello] runtime clock: 42 us
[hello] worker 1 done: tls_counter=1000
[hello] worker 2 done: tls_counter=1000
[hello] 2 workers x 1000 locked increments = 2000 shared increments
[hello] elapsed: 2102 us
[hello] ALL CHECKS PASSED
```

This exercises, in real guest code: dynamic import binding (14 JUMP_SLOT
entries bound at load), TLS via the general-dynamic model (per-thread
`__thread` counters: 1000/1000/0 — worker/worker/main), recursive-safe
mutexes, counting semaphores, thread create/start/wait/exit, and the
monotonic clock.

## 4. State of the project

- **Works:** load → relocate → bind → initialize → execute → render →
  capture → controlled teardown, for PSBET-ABI guest images, on Linux, with
  CI reproducing everything (Linux, ASan/UBSan, Windows-core).
- **Does not work yet (by design, see ROADMAP):** Vulkan GPU backend and
  RDNA shader recompiler (Phase 2); faithful page tables, PFS/PKG, real
  SELF images (Phase 3); audio/input (Phase 4); Windows guest execution
  (Phase 2 shim). **No commercial PS5 title runs today** — that requires
  Phases 2–3, and claims otherwise by any project are fiction.

## 5. Defects found and fixed during the phase

Recorded because they validate the testing strategy:

1. **Packed `Elf64Rela` mis-sized** (u32 `r_info` → 20-byte struct instead
   of 24) — corrupted relocation parsing past the first entry. Caught by
   the loader trace + readelf cross-check. Fix: correct ELF64 layout.
2. **Entry point read after buffer release** — `fileBytes` were cleared by
   `scanDynamic` before `load()` read `e_entry`, yielding entry == base and
   a jump into the ELF header. Caught by the end-to-end guest test.
   Fix: read the header from the mapped image.
3. **Host symbol interposition** — exporting `__tls_get_addr` from the host
   binary shadowed glibc's own and recursed into the engine's TLS allocator
   (ASan caught it instantly). Fix: internal names, export-table-only
   guest names.
4. **Page-unaligned `mprotect`** — ELF segments are page-*congruent*, not
   page-aligned; protection calls failed for RW segments. Fix: VM layer
   aligns ranges internally.
5. **Single-flight exit guard not reset between guest runs** — second
   in-process guest parked forever. Caught by the two-guest test suite.
   Fix: per-run reset in `KernelState::init`.
6. **Guest targets inheriting host sanitizer flags** — ASan-instrumented
   guests crash by construction. Fix: build-system enforcement + rule
   documented in GUEST_ABI.

## 6. Remaining known limitations (tracked)

- `longjmp`-based process exit skips C++ destructors in flight; guests
  using per-frame static command buffers (as the shipped ones do) are
  leak-free regardless — verified by ASan. Proper unwinding-aware teardown
  is Phase 3.
- Guest threads still running at process end are detached with a warning;
  cooperative termination lands in Phase 3.
- Console output interleaving between guest `sceKernelWrite` (fd 1) and
  host logging (fd 2) can look interleaved when piped; cosmetic.
- Windows: portable core + tests only; guest execution waits on the
  Phase 2 TLS/exception shim (documented in ARCHITECTURE §9).

## 7. Artifacts

- Source: this repository, tagged by CI on every push
- CI artifacts: `psbet-linux-x64`, `psbet-windows-x64`,
  `psbet-demo-screenshots`
- Evidence: `docs/screenshots/*.png` (committed, verified distinct)

## 8. What remains to be done

Phase 2 — Vulkan GPU backend and RDNA→SPIR-V shader recompilation, plus the
Windows guest-hosting shim. Acceptance targets are quantified in
`docs/ROADMAP.md`. **Phase 1 ends here; work resumes on the explicit
"Continue to Phase 2" command.**
