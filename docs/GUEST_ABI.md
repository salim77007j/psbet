# PSBET Guest ABI v1

This document is the normative contract between a guest image and PSBET.
It is clean-room: the names are PS5-style for familiarity, the semantics
below are PSBET's own definitions, and nothing here derives from Sony code.

---

## 1. Image requirements

A guest is a standard ELF64 image:

| Property    | Requirement                                          |
|-------------|------------------------------------------------------|
| Class/Data  | ELF64, little-endian                                 |
| Machine     | `EM_X86_64` (62)                                     |
| Type        | `ET_DYN` (PIE-style; `ET_EXEC` rejected in Phase 1)  |
| Layout      | First `PT_LOAD` covers file offset 0 **at vaddr 0** (standard PIE) |
| Entry       | `psb_entry` — a plain SysV function (`void f(void)`) |
| TLS model   | General-dynamic (`__tls_get_addr`); initial-exec (`R_X86_64_TPOFF64`) is rejected |
| OSABI       | Any; `ELFOSABI_FREEBSD` (PS5 flavour) accepted       |

Encrypted SELF containers are detected by magic and rejected with guidance —
PSBET never ships key material (clean-room policy).

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

Phase 1 notes: priorities are accepted and unused; stacks run on host
threads (guest stack switching arrives with Phase 3's faithful memory
model); waits are unbounded.

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

Phase 1 uses a unified address space: allocations are CPU-visible
immediately and `MapDirectMemory` is identity. `searchStart/searchEnd` and
`memoryType` are accepted and ignored. This is the main documented
divergence from real console semantics (typed pools land in Phase 3).

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

## 8. I/O

```c
sceKernelWrite(fd, buf, n); sceKernelRead(fd, buf, n);
sceKernelOpen(path, flags, mode); sceKernelClose(fd);
```

Thin passthrough to host fds (stdout = 1). Filesystem virtualization is a
Phase 3 deliverable; guests today see the host filesystem directly.

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

## 10. Versioning

`psbGetRuntimeVersion()` packs `major.minor.patch`. ABI v1 will not break
backward compatibility; additions are additive. Compile against
`include/psbet/psb_guest_api.h`.
