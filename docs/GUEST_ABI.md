# PSBET Guest ABI v1.2

This document is the normative contract between a guest image and PSBET.
Phase 3 (v1.2) extends v1 additively: memory model v2, GPU virtual memory,
EQUEUE, virtualized I/O, raw-syscall containment. v1 guests run unchanged.
It is clean-room: the names are PS5-style for familiarity, the semantics
below are PSBET's own definitions, and nothing here derives from Sony code.

---

## 1. Image requirements

A guest is a standard ELF64 image:

| Property    | Requirement                                          |
|-------------|------------------------------------------------------|
| Class/Data  | ELF64, little-endian                                 |
| Machine     | `EM_X86_64` (62)                                     |
| Type        | `ET_DYN` (any layout) or `ET_EXEC` (absolute vaddrs must be free below 8 GiB) |
| Layout      | First `PT_LOAD` covers file offset 0 **at vaddr 0** (standard PIE) |
| Entry       | `psb_entry` — a plain SysV function (`void f(void)`) |
| TLS model   | General-dynamic (`__tls_get_addr`); initial-exec (`R_X86_64_TPOFF64`) is rejected |
| OSABI       | Any; `ELFOSABI_FREEBSD` (PS5 flavour) accepted       |

Encrypted SELF containers are detected by magic and rejected with guidance —
PSBET never ships key material (clean-room policy). Images without a
`PT_DYNAMIC` load as static-style. `R_X86_64_IRELATIVE` is resolved by
executing the addend resolver. `PT_GNU_RELRO` regions become read-only after
relocation. The first `PT_LOAD` may sit at any vaddr (v1 required vaddr 0).

## 2. Canonical build recipe (GCC/Clang, Linux)

```bash
gcc -shared -fPIC -nostartfiles -Wl,-e,psb_entry -Wl,--build-id=none -O2 \
    -o game.elf game.c
```

- `-nostartfiles` drops the host crt (no `__libc_start_main`); PSBET runs
  `DT_INIT` / `DT_INIT_ARRAY` itself, like a dynamic linker.
- `-shared` produces `ET_DYN` and keeps undefined symbols legal.
- Do **not** pass host sanitizer flags — the guest is the "console", never
  instrument it with host tooling (enforced in PSBET's own build).

## 3. Entry, exit, threads

```c
void psb_entry(void);                 // called once on the main thread
sceKernelExitProcess(int32_t status); // ends the process; exit code surfaces
// v1.1 semantics: callable from ANY guest thread. A worker's exit unwinds
// that worker's own frames (it stays joinable), wakes every blocking
// primitive, and the supervisor delivers the FIRST recorded exit code.
                                      // to the host CLI / test harness
typedef int32_t (*PsbThreadEntry)(size_t arglen, void* argp);
sceKernelCreateThread(name, entry, prio, stackSize, attr, affinity, &tid);
sceKernelStartThread(tid, arglen, argp);   // argp copied, passed to entry
sceKernelWaitThreadEnd(tid, &retVal, 0);   // unbounded in v1
sceKernelExitThread(retVal);               // unwinds ONLY the calling thread
```

Priorities are accepted and unused; waits are unbounded. v1.2 semantics:
`sceKernelExitThread` and `sceKernelExitProcess` are safe from ANY guest
thread — each unwinds only the calling thread's own frames (there is no
cross-thread unwinding anywhere in PSBET since the Phase-3 hosting-model
unification).

## 4. TLS

Use normal `__thread` variables. The compiler will emit general-dynamic
relocations (`DTPMOD64` + `DTPOFF64`) and `__tls_get_addr` calls; PSBET
resolves `__tls_get_addr` to its own allocator and materializes per-thread
blocks from the image's `PT_TLS` segment. Host TLS is never shared with the
guest. Constraint: TLS variables with alignment > 16 bytes are not yet
supported.

## 5. Memory

```c
sceKernelAllocateDirectMemory(searchStart, searchEnd, len, alignment,
                              memoryType, &physAddrOut);
sceKernelMapDirectMemory(&addrOut, len, prot, flags, physAddr, pageSize);
```

v1.2 (Phase 3) — the faithful model:
```c
sceKernelAllocateDirectMemory(searchStart, searchEnd, len, alignment,
                              memoryType, &physAddrOut); // typed pools; search range honoured
sceKernelFreeDirectMemory(physAddr, len);                 // refcount-guarded
sceKernelMapDirectMemory(&addrOut, len, prot, flags, physAddr, pageSize);
sceKernelMapNamedDirectMemory(&addrOut, len, prot, flags, physAddr, pageSize, name);
sceKernelMmap(&addrOut, len, prot, flags, fd, offset);    // anonymous only
sceKernelMunmap(addr, len);
sceKernelQueryMemoryProtection(addr, &startOut, &endOut, &protOut);
sceGnmReserveGpuAddress(len, alignment, fixedGpuVa, &gpuVaOut);
sceGnmMapGpuAddress(gpuVa, physAddr, len, prot);
sceGnmUnmapGpuAddress(gpuVa, len);
```
* `memoryType`: 0 = regular pool, 1 = GPU-visible pool (other values warn
  and use the regular pool).
* CPU mappings are identity (VA = arena + PA) — native-execution model.
  `PSB_MAP_FIXED` requires the caller's address to equal the identity
  mapping; VA≠PA CPU remapping is rejected (documented divergence).
* Protection bits: `PSB_PROT_READ 0x1`, `PSB_PROT_WRITE 0x2`,
  `PSB_PROT_EXEC 0x4`.
* The GPU owns a separate virtual address space: reserve a window, map
  physical pages into it, then reference the GPU-VA from PBFF v1.1 packets
  (opcode 6). GPU VAs are translated through real page tables per submit.

## 6. Time

```c
sceKernelGetProcessTime();        // microseconds since process start
sceKernelGetProcessTimeInTick();  // 1 tick = 1 nanosecond (PSBET definition)
sceKernelUsleep(us); sceKernelNanosleep(s, ns); sceKernelSleep(s);
```

Clock is monotonic (`steady_clock`).

## 7. Synchronization

```c
sceKernelCreateMutex(name, attr, initLocked, &id);      // v1: recursive-safe
sceKernelLockMutex(id, 0 /*unbounded*/); sceKernelUnlockMutex(id);
sceKernelCreateSemaphore(name, init, max, &id);
sceKernelWaitSemaphore(id, need, 0); sceKernelSignalSemaphore(id, count);
sceKernelCreateCond(name, attr, &id); sceKernelWaitCond(id, mutexId, 0);
sceKernelSignalCond(id); sceKernelSignalCondAll(id);
```

## 8. I/O (v1.2: virtualized filesystem)

```c
sceKernelWrite(fd, buf, n); sceKernelRead(fd, buf, n);
sceKernelOpen(path, flags, mode); sceKernelClose(fd);
sceKernelLseek(fd, offset, whence);
typedef struct PsbStat { uint64_t size; uint32_t isDir; uint32_t readOnly; } PsbStat;
sceKernelFstat(fd, PsbStat* out);
```

Guests never touch the host filesystem. Mounts: `app0:/` — a read-only
container image (PSBET Archive; pack one with `psbet mkfsa <dir> <out>`,
mount with `--mount-pfs`); `/data` — a writable host directory
(`--data-dir`). Paths outside the mounts never resolve. Open flags:
RDONLY 0x0, WRONLY 0x1, RDWR 0x2, CREATE 0x40, TRUNC 0x200. fds 0/1/2 are
the host console (serialized).

## 9. GPU submission

```c
sceGnmSubmitCommandBuffers(count, dcbAddrs[], dcbSizes[], ccbAddrs[],
                           ccbSizes[], flags);
```

Guests build display command buffers (PBFF v1 — `gpu/cmd_stream.h` in the
runtime tree defines every packet: `Clear`, `SetViewport`, `SetDepthTest`,
`SetCullMode`, `DrawTriangles` with NDC `pos[4]` + `col[4]` vertices) and
submit them each frame. Malformed streams are rejected, not executed.
Vertex positions are NDC with `z` as depth in `[0,1]` (1 = far); the vertex
"shader stage" is host-side identity in v1 — matrix math runs on the guest
CPU. Real RDNA shader execution is the Phase 2 deliverable.

Per submit, PSBET runs the frame hook: capture (PNG at interval), FPS
accounting, and the safety frame limit (`--frames`; on breach the guest is
exited with code 124).

## 10. Raw syscalls (v1.2 containment)

A guest that executes a raw `syscall` instruction would reach the HOST
kernel — PSBET closes that hatch on Linux: guest threads run under a
seccomp trap; host-escaping syscalls (exec, fork, sockets, mounts, modules,
keyrings, ...) are refused with `-EPERM` and logged. Benign syscalls stay
allowed. On platforms without seccomp the boundary is the emulated import
surface itself. Guests should always use the `sce*` surface — raw syscalls
are neither portable nor stable.

## 11. Event queues (v1.2)

```c
sceKernelCreateEqueue(name, &eq); sceKernelDeleteEqueue(eq);
sceKernelAddEvent(eq, ident, flags, userData);     // guest-side post
sceKernelTriggerEvent(eq, ident, flags, userData); // kernel-side signal (same effect in v1.2)
sceKernelWaitEqueue(eq, &eventOut, timeoutUs);     // 0 = unbounded; wakeable by exit/shutdown
sceKernelCancelEvent(eq, ident);
```

## 12. Versioning

`psbGetRuntimeVersion()` packs `major.minor.patch`. ABI v1 will not break
backward compatibility; additions are additive. Compile against
`include/psbet/psb_guest_api.h`.
