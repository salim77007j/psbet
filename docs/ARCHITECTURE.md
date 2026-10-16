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
