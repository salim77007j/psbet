# PSBET Architecture

This document is the technical design record of PSBET. It explains *why* the
system is shaped the way it is, what Phase 1 actually implements, and where
the deliberate simplifications live. Everything described here is clean-room
work: PS5 behavior is reimplemented from public documentation, with original
code and original internal formats.

---

## 1. Strategy decision record

### 1.1 The defining constraint

The PS5 is not exotic hardware to a PC. Its CPU is an 8-core/16-thread AMD
Zen 2 running x86-64 (the SysV ABI, FreeBSD flavour), its GPU is an AMD RDNA
2-class device, and its kernel is a FreeBSD derivative. That shapes the
entire strategy:

**Decision D1 — native execution, not CPU emulation.**
Emulating x86-64 on x86-64 (an interpreter or binary translator like
QEMU/Box64) wastes an order of magnitude of performance to solve a problem
this platform doesn't have. PSBET instead loads guest images into a managed
address space and executes them directly on the host CPU, intercepting every
*outward-facing* operation (kernel calls, GPU submission) at the API
boundary. This is the WINE model applied to a console — and it is the same
reasoning that made shadPS4 viable on the PS4 side of this family.

Consequences:
- Guest code runs at full native speed (Phase 1 measures ~5.9 ms/frame for a
  full 720p software-rasterized cube scene — CPU-side work is negligible).
- The hard problems move to where they actually are: the OS surface
  (`libkernel`, `libc`), the GPU interface (command buffers, shaders), and
  fidelity of the memory model.
- Host OS differences matter: Linux hosts execute guests directly in Phase 1;
  Windows hosts get the portable core (memory/rasterizer/PNG/CI) while a
  thin "guest-hosting" shim (TLS + structured exception mapping) lands in
  Phase 2. This is scoped work, not an architectural blocker.

### 1.2 Why C++17 with zero dependencies

**Decision D2 — C++17, no third-party libraries in Phase 1.**
The core needs exactly four OS primitives (mmap/mprotect/munmap, VirtualAlloc
equivalents), threads, and dynamic symbol lookup. Wrapping them directly
keeps the engine auditable (important for the clean-room claim), makes CI
trivial on both platforms, and avoids versioning a dependency tree before
the architecture is proven. The PNG writer, command format, and rasterizer
are all self-contained for the same reason. Re-evaluation points: the
Phase 2 Vulkan backend will introduce `vulkan-headers` only; the Phase 4
audio/input layer will introduce SDL3.

### 1.3 What Phase 1 does NOT attempt

- Encrypted SELF/PKG parsing (explicitly rejected with guidance; clean-room
  policy forbids shipping key material).
- Commercial game compatibility. Real titles require the Orbis OS layer at
  depth (Phase 3) and the GPU translation layer (Phase 2). Anyone claiming
  "PS5 games run flawlessly on PC" today is selling fiction; PSBET's roadmap
  is the honest path there.
- Full PS5 memory-map fidelity (page tables, typed pools, GPU page tables).
  Phase 1 uses a documented unified model; see §3.

---

## 2. System overview

```
                      ┌──────────────────────────────────────────┐
                      │                psbet CLI                 │
                      │        run / info / capture options      │
                      └───────────────────┬──────────────────────┘
                                          │
                      ┌───────────────────▼──────────────────────┐
                      │               Emulator                   │
                      │   orchestration, frame capture, stats    │
                      └───┬──────────┬──────────┬───────────┬────┘
                          │          │          │           │
             ┌────────────▼───┐ ┌────▼─────┐ ┌──▼───────┐ ┌─▼──────────┐
             │ MemoryManager  │ │ ElfLoader│ │ libkernel│ │ GPU Device │
             │ guest arena    │ │ ELF64,   │ │ shim:    │ │ PBFF cmd   │
             │ direct memory  │ │ reloc,   │ │ threads, │ │ executor → │
             │ protections    │ │ TLS,     │ │ sync,    │ │ software   │
             │                │ │ imports  │ │ time, I/O│ │ rasterizer │
             └────────────────┘ └────┬─────┘ └──────────┘ └─────┬──────┘
                                     │                          │
                             ┌───────▼────────┐          ┌──────▼──────┐
                             │ CPU supervisor │          │ PNG capture │
                             │ entry/exit,    │          │ (evidence)  │
                             │ crash reports  │          └─────────────┘
                             └────────────────┘
```

Execution flow for one guest:

1. **Memory init.** A 16 GiB guest arena is reserved (`PROT_NONE`) at a high
   address hint. Allocations commit pages with explicit protections.
2. **Load.** The ELF64 image is validated (class, endianness, machine,
   ET_DYN, SELF-magic rejection), mapped segment-by-segment with
   R/W/X enforcement, and its `PT_DYNAMIC` table is scanned.
3. **Relocate.** `R_X86_64_RELATIVE / _64 / GLOB_DAT / JUMP_SLOT` are applied
   bind-now; TLS relocations (`DTPMOD64/DTPOFF64/DTPOFF32`) map to PSBET's
   TLS allocator; unsupported types fail loudly with guidance.
4. **Bind imports.** Undefined symbols resolve in order: kernel shim → GPU
   shim → host runtime (`dlsym`) — with weak/ignorable-symbol semantics.
5. **Initialize.** `DT_INIT`, then `DT_INIT_ARRAY` (dynamic-linker order).
6. **Execute.** The CPU supervisor installs main-thread TLS and calls the
   entry point. Guest exit (`sceKernelExitProcess`) or a safety frame limit
   returns control via a controlled `longjmp`.
7. **Capture.** Each GPU submit increments the frame counter; a host-side
   hook dumps the framebuffer as a PNG at the configured interval.

---

## 3. Memory model (Phase 1)

| Property          | Real PS5                          | PSBET Phase 1                      |
|-------------------|-----------------------------------|------------------------------------|
| Address space     | 48-bit VA, GPU/CPU separate PTEs  | One 16 GiB arena, unified pointers |
| Direct memory     | Typed physical pools + GPU VM     | Bump-allocated, CPU-visible        |
| Protections       | Per-page, per-process             | Per-region (mprotect/VirtualAlloc) |
| Stack             | Per-thread, kernel-allocated      | Host thread stacks (documented)    |

The unified model is a *deliberate* Phase 1 simplification: guest pointers
are host pointers, which makes the loader, shim, and rasterizer provably
correct while the hard parts (GPU command format, TLS, threading) are
proven. Phase 3 replaces it with a faithful page-table model; the code is
isolated behind `MemoryManager` precisely so that swap is local.

Region protections are enforced by the OS: executable images carry RX on
code, R on rodata, RW on data (W^X respected at commit time), and the arena
is reserved `PROT_NONE` before use, so wild guest pointers die loudly with a
diagnostic rather than silently corrupting.

## 4. Loader semantics

The loader implements the subset of the ELF64 dynamic-linking contract that
PS5-style images actually use, with a documented guest-ABI contract
(first `PT_LOAD` at vaddr 0, `ET_DYN`, plain SysV entry):

- **Validation**: magic/class/endianness/machine/type; SELF magic rejection
  with user guidance; program-header sanity bounds.
- **Mapping**: segments copied with `p_filesz` bytes and zero-filled to
  `p_memsz`; per-segment protections applied page-granularly (segments are
  only page-*congruent*, so the VM layer aligns internally).
- **Relocations**: RELATIVE / 64 / GLOB_DAT / JUMP_SLOT / DTPMOD64 /
  DTPOFF64 / DTPOFF32 supported; IRELATIVE and TPOFF64 rejected with
  actionable messages (guests use general-dynamic TLS — see GUEST_ABI).
- **TLS**: the `PT_TLS` image is registered as module 1; every guest thread
  materializes its own block on first use through `__tls_get_addr`
  semantics implemented in the shim (host TLS is never touched — this is
  what keeps host and guest worlds isolated).
- **Imports**: bound eagerly; resolution order kernel → GPU → host libc;
  weak/`_ITM_*` references bind to null per ELF semantics.
- **Initializers**: `DT_INIT` then `DT_INIT_ARRAY`, matching dynamic-linker
  order.

A structural-debugging note recorded for posterity: the loader's unit
failure history (packed-struct `Elf64Rela` at 20 instead of 24 bytes; entry
point read from a released file buffer; page-unaligned `mprotect` calls) was
all caught by the test suite and the ASan build — exactly the failure classes
the Phase 1 harness was built to catch.

## 5. Kernel shim (`libkernel` emulation)

33 exported functions with PS5-style names and PSBET-defined semantics
(`docs/GUEST_ABI.md` is normative):

- **Memory**: `sceKernelAllocateDirectMemory` (bump allocation),
  `sceKernelMapDirectMemory` (identity in the unified model),
  `sceKernelGetDirectMemorySize`.
- **Threads**: create/start/wait/exit with Orbis-style
  `arglen/argp` entry; guest threads run on host `std::thread`s with their
  own TLS blocks; `sceKernelExitThread` unwinds exactly one guest thread via
  a per-thread `jmp_buf` trampoline.
- **Synchronization**: recursive-safe mutexes, counting semaphores
  (mutex+CV), condition variables.
- **Time**: monotonic process time (µs) and ticks (1 tick = 1 ns, documented
  divergence from hardware counters).
- **I/O**: thin fd passthrough for console-style `sceKernelWrite` etc.
- **Process exit**: single-flight guard; `longjmp` back into the supervisor
  from the main thread; racing second-exit requests park (deterministic
  teardown, no cross-thread unwinding).

**Isolation rule (learned the hard way, codified):** the shim never defines
host-privileged symbols. `__tls_get_addr` / `__cxa_finalize` exist only as
*export-table names* mapped to internally-named host functions — defining
them as real host symbols would interpose glibc's own for the entire process
and corrupt ASan/libstdc++.

## 6. GPU subsystem

**Command format (PBFF v1)** — a clean-room packet stream: tagged headers
with explicit sizes (malformed input is rejected, not trusted), packets for
clear / viewport / depth state / cull state / triangle draws with NDC
positions + RGBA colors. The layout intentionally mirrors the *shape* of
console submission (ring of packets, `sceGnmSubmitCommandBuffers` entry
point) so Phase 2 can grow real RDNA command translation behind the same
guest-visible surface.

**Software rasterizer** — the reference backend: edge-function
rasterization with watertight fill (`E >= 0`), depth test (LESS/LEQUAL),
backface culling, viewport transform, barycentric color interpolation,
deterministic output. Runs headless — which is what makes the CI "render a
frame and screenshot it" pipeline possible with zero GPUs.

**Performance envelope (measured, 1280×720)**: ~140–170 FPS rendering the
12-triangle rotating cube on one core, including per-frame command
validation. The rasterizer is deliberately simple; it is the correctness
oracle for Phase 2's Vulkan backend, not the performance story. Phase 2
targets: command-buffer translation to Vulkan, RDNA ISA → SPIR-V shader
recompilation, pipeline + shader caches, async compute, and a GPU-side
framegraph.

## 7. CPU supervisor & diagnostics

- Guest entry is called under a supervisor `jmp_buf`; `sceKernelExitProcess`
  and the GPU frame-limit both land in the same controlled path.
- Fatal signals (`SIGSEGV/SIGBUS/SIGILL/SIGFPE`) produce a report containing
  the faulting address translated into *guest module offsets* — the single
  most important metric when triaging compatibility later.
- Crash-during-guest policy: no unwinding through guest frames; report and
  `_Exit(139)` with a distinctive code.

## 8. Testing & verification strategy

- **Unit layer**: memory manager, PNG encoder (CRC32/Adler32 vectors +
  structure), rasterizer (coverage, depth ordering), command executor
  (valid + malformed streams).
- **Integration layer**: full `Emulator::run` over real guest ELFs;
  `hello.elf` asserts thread-safety and TLS isolation numerically
  (2×1000 locked increments, per-thread TLS counters) and returns its result
  as the guest exit code; `cube.elf` proves the GPU pipeline end-to-end with
  screenshot count assertions.
- **Sanitizer layer**: the entire host engine runs clean under
  ASan+UBSan with leak detection; guests are explicitly *not* instrumented
  (they are the "console", not the host — enforced in the build).
- **Evidence layer**: every CI run produces downloadable binaries and real
  rendered screenshots; deterministic renderer makes pixels reviewable.

## 9. Risk register (top items)

| Risk | Impact | Mitigation / plan |
|------|--------|-------------------|
| Windows guest hosting (TLS via FS base, SEH) | Blocks one platform | Phase 2 shim: thread-local guest TLS base switching + vectored exception mapping; portable core already ships |
| Detached guest threads at exit | Late-process races | Phase 3: cooperative thread termination + join-all on exit |
| `longjmp` across frames with nontrivial destructors | Host leaks on forced exit | Guests use static command buffers (already true for cube); Phase 3 adds unwinding-aware teardown |
| RDNA shader recompiler scale | The largest engineering item | Phase 2 starts with compute-free vertex/fragment path over Vulkan; ISA disassembler is standalone and testable |
| Commercial-title long tail | Compatibility work is open-ended | Crash-report offsets + capture/replay harness (Phase 6) turns anecdotes into regression tests |


---

## 10. Phase 2 addendum — GPU translation layer

```
guest PBFF command buffer
        │
        ▼
  Device (parse + validate)          ── one shared surface
        │
        ├── IRasterBackend::software → Rasterizer (reference oracle)
        └── IRasterBackend::vulkan   → VulkanDevice
                │  loader-based (no SDK), offscreen images
                │  fixed-equivalent pipeline (SPIR-V built by spirv_emit)
                │  shader cache (disk, CRC-checked) + VkPipelineCache
                └── RDNA subset → SPIR-V recompiler (rdna_spirv)
                        └── docs/RDNA_SUBSET.md is the normative contract
```

Backend selection is per-run (`--backend auto|software|vulkan`). Before any
guest runs, the Vulkan path self-tests (render a known triangle under a
signal guard, verify readback) and falls back to software on failure — a
broken host driver can never take the process down or silently corrupt
output. Risk register addition: llvmpipe JIT faults inside
`vkCreateGraphicsPipelines` on some sandboxed kernels (observed, contained by
the self-test; healthy runners/PCs unaffected).

---

## 11. Phase 3 addendum — Orbis OS depth & memory fidelity

### 11.1 Memory model v2 (guest_vm)

```
physical address space (PSBET PA == arena offset)
 ┌───────────────────────────────────────────────────────┐ 16 GiB reservation
 │ page 0: null guard (never allocatable)                │
 │ [0x1000, 8 GiB)   pool 0: REGULAR (CPU+GPU)           │ free-list per pool,
 │ [8 GiB, 12 GiB)   pool 1: GPU-VISIBLE                 │ Orbis-style search
 │ [12 GiB, 16 GiB)  headroom                            │ ranges + coalescing
 └───────────────────────────────────────────────────────┘
        │ identity CPU mapping (VA = arena + PA)          │ GPU VM (separate!)
        ▼                                                 ▼
 CPU page tables                                   GPU page tables
 2-level, 4 KiB leaves under 2 MiB mids            same structure, own VA space
 PTE: present|prot|PA-frame                        8 GiB window, reserve/map/unmap
 enforced by host mprotect                         translated per GPU-VA access
```

Design contract (documented divergence, enforced loudly):
* PSBET executes guests natively — a guest CPU pointer **is** a host pointer.
  CPU direct-memory mappings are therefore identity (VA = arena + PA); the
  *model* beneath (typed pools, search ranges, PTEs, refcounted mappings) is
  faithful, and the GPU owns a fully separate virtual address space with a
  real page-table walk (never assumed identity).
* CPU-side VA≠PA remapping (aliasing one physical allocation at two CPU
  addresses) is **rejected** in v0.3: it buys nothing until framebuffer
  aliasing (Phase 5) and would break the native-pointer invariant.
* PA 0 is a reserved null-guard page: wild null-pointer guests die loudly.
* `freeDirect` is refcount-guarded — memory in use by any live CPU or GPU
  mapping cannot be freed (double-map, double-free and use-after-free of
  direct memory are hard errors, fuzz-locked).

### 11.2 GPU virtual memory (PBFF v1.1)

Guests reserve a GPU-VA window (`sceGnmReserveGpuAddress`), map physical
allocations into it (`sceGnmMapGpuAddress`), and draw through the mapping:
PBFF opcode 6 (`DrawTrianglesGpu`) carries only the GPU-VA; the device
translates the address through the GPU page tables per submit (whole-span
validation; unmapped or partially-mapped spans reject the buffer). The
`cube_vm` guest renders the Phase-1 cube entirely through this path —
screenshots in `docs/screenshots/phase3_cube_vm_*.png`.

### 11.3 Virtualized filesystem (vfs + pfs)

Guests no longer see the host filesystem. The VFS exposes:
* `app0:/` — a read-only mounted container image. Phase 3 ships the
  **PSBET Archive (PSA)** format (own clean-room container: header, bounds-
  checked entry table, name pool, data region) plus a `psbet mkfsa` packer.
  Encrypted Sony PFS containers remain unsupported (clean-room policy: no
  key material; the public structural documentation base was insufficient
  for a verified reader at Phase-3 time — see PHASE_3_REPORT §5).
* `/data` — a writable passthrough backed by one host directory
  (`--data-dir`).
* Everything else — **nothing**. Guest paths outside the mounts return
  errors; `/etc/passwd` does not exist to a guest. Kernel fds 0/1/2 stay
  host std streams (serialized console), fds ≥ 3 are VFS-owned.

### 11.4 Raw-syscall containment (syscall_trap)

Native execution means a guest `syscall` instruction reaches the *host*
kernel — the one escape hatch around the emulated surface. Phase 3 closes it
on Linux: every thread that executes guest code installs a per-thread
seccomp filter (SECCOMP_RET_TRAP) over a deny-list of host-escaping syscalls
(exec/fork, sockets, mounts/namespaces, modules, keyrings, kexec, io_uring,
bpf, process_vm, ...). Trapped calls raise SIGSYS; PSBET's handler answers
-EPERM (logged) and resumes the guest. Filters attach to guest threads only
— engine threads never execute guest code, so host tooling (ASan/LSan) is
unaffected. On platforms without seccomp the documented fallback is the
emulated import boundary itself (GUEST_ABI §11).

This forced a hosting-model unification: the guest main now runs on a
dedicated thread on **both** platforms (previously POSIX ran it on the
supervisor thread). Benefits beyond containment: zero cross-thread longjmp
in the codebase (the Phase-1 flaw class is now structurally impossible),
engine teardown never unwinds through guest frames, and "every thread that
runs guest code is filtered" is exactly true by construction.

### 11.5 Kernel surface v3 (GUEST_ABI v1.2)

Additions over v1.1: `sceKernelFreeDirectMemory`, `sceKernelMapNamedDirectMemory`,
`sceKernelMmap`/`sceKernelMunmap` (anonymous), `sceKernelQueryMemoryProtection`,
`sceKernelLseek`/`sceKernelFstat` (VFS-routed I/O), the EQUEUE set
(`sceKernelCreateEqueue`/`DeleteEqueue`/`AddEvent`/`TriggerEvent`/
`WaitEqueue`/`CancelEvent` — wakeable waits, cooperative on exit/shutdown),
and the GPU VM set (`sceGnmReserveGpuAddress`/`MapGpuAddress`/
`UnmapGpuAddress`). Teardown accounting is now measured and reported
(joined / force-detached / parked per run; the CLI prints it, tests assert
zero force-detached on POSIX).

### 11.6 Windows ABI conformance (the Phase-2 residual, root-caused)

The Phase-2 report tracked a residual "garbled counters → stack scribble →
AV at exit" on Windows. Root cause found by code inspection in Phase 3:
`w64_thunks.asm` `thunk3` executed `mov r8, rdx` **after** `mov rdx, rsi`
had clobbered rdx — every 3-argument guest→host call delivered arg1 twice
and lost arg2 (`sceKernelStartThread` got a garbage argp, `sceKernelWrite`
a garbage length, `sceKernelCreateCond` wrote its handle through a garbage
out-pointer). Fixed; a new thunk7 class was added (SysV passes 6 register
args, Win64 only 4 — SysV register args 4–5 become Win64 *stack* args); and
the `abi_probe` guest now regression-locks every thunk arity (0–6 + pointer
args) as a hard CI gate on Windows. `sceKernelExitThread` on Windows was
also corrected: it now records the result and parks (never unwinding GCC
frames under MSVC SEH), with `WaitThreadEnd` returning the recorded value
without joining a parked thread.

### 11.7 Fuzzed guest corpus

`psbet_fuzz` (CI-gated, ASan-clean): deterministic seeded workloads over the
memory model (alloc/map/verify/unmap/free with invariant checks, allocator
reuse and coalescing under churn) and over the PBFF executor (byte-level
mutations of valid streams, fully random streams, valid-random streams that
must always render). 64 seeds per CI run; exit code = failed seeds.

### 11.8 Risk register update

| Risk | Status after Phase 3 |
|------|----------------------|
| Windows guest hosting | ABI root-caused + conformance-gated; park accounting in place |
| Detached threads at exit | Zero force-detached on POSIX (measured, test-locked) |
| `longjmp` across guest frames | Structurally eliminated (no cross-thread unwinding exists) |
| RDNA recompiler vs real captures | Unchanged (Phase 4+, needs captures) |
| Commercial-title long tail | Unchanged (Phase 6 program) |

## 12. Phase 4 addendum — audio, input, timing, pacing, presentation

### 12.1 Time domain (the single guest clock)

Every guest timing API derives from one `TimeDomain`: `value = origin +
(now − start)` on the host steady clock, or an injected virtual offset in
test mode. There is no accumulation and no re-derivation against a second
clock, so the two classic drift sources of emulated clocks cannot occur.
The counter API (`GetProcessTimeCounter` + fixed 1 GHz frequency) serves
the same domain. Sleeps are cooperative: they slice at 2 ms and wake early
on exit/shutdown — a sleeping guest can no longer stall teardown (the last
blocking primitive that could).

The 24-hour simulated soak runs the domain in virtual mode: 86,400 seconds
are injected in 100 ms steps with exact-value assertions (drift-free by
construction is verified, not assumed), plus concurrent readers asserting
monotonicity.

### 12.2 AudioOut: shared master timeline with per-port cursors

The mixer owns ONE master ring (65,536 frames ≈ 1.36 s, interleaved stereo
i32 accumulators). Each port has a write cursor that advances only with its
own submits; ports submitting in the same frame window therefore SUM into
the same master frames (Orbis-style parallel mixing), while staggered
submits lay out sequentially. Volume (per port per channel, 0..32768) and
format conversion (s16 mono/stereo, f32 stereo) apply at write time; a
single clamp happens at read time.

Consumption models:
* **soak** (no pacing) — every submit drains the written region immediately
  (conservatively: never past the minimum live port cursor, so a second
  port's same-window submit is never misclassified as late);
* **paced** (`--fps N`) — each presented frame authorizes exactly
  (48000/N) master frames; audio is glued to video pacing;
* **SDL3** (optional build) — the same rules; SDL consumes by wall clock on
  a real/dummy device.

Backpressure: a submit blocks (guest-cooperative 1 ms slices) while the
unplayed frontier is at/over the 30 ms cap, with a 50 ms liveness bound —
under the frame-clock model the presenter is often the same guest thread,
so a submit that cannot be resolved by the drain must not wait forever.
The read position never advances past the written frontier (an over-advance
would underflow every unsigned room-check into "forever full" — found by
the paced tone run, documented here as the reason the advance is clamped
and underruns are counted instead).

Dropped samples are an invariant: 0. The only drop path is a port
submitting into an already-played region (genuinely late), which is
counted in stats, never silent. The fuzz corpus (Phase F) exercises random
port open/output/volume/close sequences and asserts the invariant per run.

### 12.3 Pad: driver-model input with a latency contract

`PadService` serves Orbis-shaped `scePad*` reads from a pluggable driver:
`NullPadDriver` (all disconnected), `ScriptedPadDriver` (deterministic
time-keyed events from a text script — CI and reproducible bug reports),
and `SdlGamepadDriver` (SDL3 game controllers + gyro; real DualSense
mapping). Every state carries `timestampUs` (when it became valid), so
end-to-end input latency = observation − timestamp is measurable inside the
guest. At 60 FPS pacing the input_demo guest measures and enforces the
< 25 ms budget itself; the CI suite asserts it end-to-end.

### 12.4 Pacing, vsync, presentation

`FramePacer` implements target-FPS authorization (the console boost-mode
analogue: `--fps 60|120`) on an ABSOLUTE schedule (frame N presents at
start + N·period) — pacing error cannot accumulate. Vsync is modelled as
present-queue backpressure (depth 2) with honest scoping: under Phase 4's
synchronous submit path it degenerates to pacing and its wait counter
reads 0; it becomes measurable when command recording pipelines (Phase 5).
Presentation goes through an `IPresenter`: offscreen (default, CI) or the
SDL3 window + streaming texture (works under the CI dummy video driver,
real display on user machines). Frame-interval statistics (mean/min/max/
p99) are reported in every run summary.

### 12.5 Net stubs

The `sceNet*` surface is fully stubbed: every call is rate-limited-logged
(first 8 in full, then aggregated) and refused with documented codes;
`sceNetCtlGetState` honestly reports disconnected. Nothing opens sockets
or touches the host stack — auditable by construction. Counters are
exposed to the run summary.

### 12.6 Risk register update

| Risk | Status after Phase 4 |
|------|----------------------|
| Audio fidelity vs real devices | SDL3 path verified via dummy driver + WAV-dump evidence; real-speaker validation is user-side |
| Pacing under pipelined submits | Vsync queue depth ready; measurement lands with Phase 5 recording |
| RDNA recompiler vs real captures | Unchanged (Phase 4+ per RDNA_SUBSET.md) |
| Net-dependent guests | Deterministic refusal + logging; guests can gate on link state |
| Commercial-title long tail | Unchanged (Phase 6 program) |

## 13. Phase 5 addendum — performance engineering

### 13.1 Pipelined GPU submit (the queue architecture)

Phase 1-4 executed every guest submit inline on the guest thread: build CB
-> parse -> draw -> present -> pace, all inside `sceGnmSubmitCommandBuffers`.
Phase 5 splits that along the console's own seam. The `Device` owns two
`GpuQueue` instances (graphics, compute), each a FIFO with a dedicated
worker thread, a bounded snapshot arena, and an in-flight cap of 2:

```
guest thread                         GPU worker (graphics)
-----------                          ----------------------
build CB #N  ---- submit ----------> [snapshot N]
build CB #N+1 <---- returns          execute N (parse + draw + present)
build CB #N+2 ...                    execute N+1
   (blocked at 2 in flight)          ...
```

Determinism: the queue is FIFO and single-worker, so a submit's state
changes (viewport, depth, cull) can never interleave with another submit's
draws — output is identical to the synchronous path for any timing. The
in-flight cap doubles as the work-ahead bound and the natural backpressure:
the guest may be at most 2 frames ahead of the GPU, and a submit blocked on
the cap wakes on process exit or the frame limit (no teardown hangs).

Console hazard model preserved: the device reads GPU-visible memory at
execution time, so guests that mutate submitted data must multi-buffer
(cube_vm now triple-buffers its vertex slots — 3 >= cap 2 + 1) or fence via
`sceGnmWaitQueueIdle`. This is the documented console contract, not an
emulator invention.

`--gpu-sync` restores inline execution; unit tests default to synchronous
and opt into pipelined mode explicitly, and the whole 37-test suite runs
green in both modes.

### 13.2 Async compute queue

The compute queue is a second worker with the same mechanics. The Phase-5
compute surface is `CmdFillMemory` (PBFF v1.4, op 7): a data-parallel fill
of a GPU-VA region — the canonical first compute dispatch. Cross-queue
ordering is explicit (`sceGnmWaitQueueIdle(PSB_QUEUE_COMPUTE)`); the
`compute_fill` homebrew proves fill -> fence -> verify -> graphics-consume
end to end, and `mt_submit` proves four guest threads can submit
concurrently with exact frame accounting (fuzz Phase-G hammers it).

### 13.3 Band-parallel software rasterizer + AVX2

The reference rasterizer hoists per-triangle setup (NDC->screen, edge
deltas, bbox) and splits each large triangle's bounding box into disjoint
row bands executed on a persistent worker pool (default: min(hw-1, 8),
`PSBET_RASTER_THREADS` override). Bands own disjoint pixels, so output is
byte-identical for any thread count. An optional AVX2 path evaluates four
pixels per vector with the exact same IEEE double operations (explicit
intrinsics, no FMA contraction, scalar tail) — byte-identical to the scalar
path, locked by `raster_parallel` / `avx2_compare` tests. Dispatch:
`PSBET_RASTER_SIMD=off` forces scalar for A/B measurement.

Measured on the 2-vCPU Phase-5 build host (4K cube, 300 frames, worker
execution time): scalar/1 thread 9.7 ms/frame; AVX/1 thread 5.9 ms (1.64x);
AVX/2 threads 3.9 ms (2.5x total). 4K output of the demo suite runs at
~170-256 FPS on this 2-vCPU container — already past the >= 120 FPS
acceptance before any dedicated reference hardware.

### 13.4 Host-side tuning + lazy readback

The Vulkan backend skips its per-frame readback copy when the host color
buffer is not consumed (no screenshots, offscreen present) — a full
framebuffer copy saved per submit. Tuning knobs: `--gpu-sync`,
`--gpu-threads N`, `PSBET_RASTER_THREADS`, `PSBET_RASTER_SIMD`.

### 13.5 Performance regression tracking

`psbet bench` (software 720p, software 4K, software GPU-VA 720p, advisory
Vulkan case) emits a JSON report; `ci/perf_gate.py` enforces
`ci/perf_budgets.json` floors per commit and publishes the report as an
artifact. Policy: shared-runner floors catch >>10% regressions; the strict
2% rule is the same-machine A/B procedure (bench before/after, diff). The
report documents the measured baseline and the tightening procedure.
