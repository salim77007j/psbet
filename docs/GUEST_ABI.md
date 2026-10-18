# PSBET Guest ABI v1.3

> **v1.3 (Phase 4) additions:** the timing-counter API, AudioOut
> (guest-side mixing), pad input (DualSense-style state + gyro), and the
> stubbed network surface. All additions are additive; v1/v1.2 guests run
> unchanged. See §6, §13, §14, §15 below.

This document is the normative contract between a guest image and PSBET.
Phase 3 (v1.2) extends v1 additively: memory model v2, GPU virtual memory,
EQUEUE, virtualized I/O, raw-syscall containment. Phase 4 (v1.3) adds audio,
input, the timing counter pair and net stubs. v1 guests run unchanged.
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

## 6. Time (v1.3: counter pair)

```c
sceKernelGetProcessTime();        // microseconds since process start
sceKernelGetProcessTimeInTick();  // 1 tick = 1 nanosecond (PSBET definition)
sceKernelGetProcessTimeCounter();          // counter ticks (1 tick = 1 ns)
sceKernelGetProcessTimeCounterFrequency(); // fixed: 1,000,000,000 Hz
sceKernelUsleep(us); sceKernelNanosleep(s, ns); sceKernelSleep(s);
```

The clock is **monotonic and drift-free by construction**: every value is
derived from one origin sample taken at process start (`value = origin +
elapsed`), with no accumulation and no re-derivation against a second clock.
Counter frequency is a fixed constant, not a measured property. Sleeps are
cooperative: they wake early (returning 0) when the process exit is requested
or engine shutdown begins, so a sleeping guest can never stall teardown.

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

## 13. Audio out (v1.3)

PSBET mixes every open port into ONE fixed master stream: **48 kHz, stereo,
s16**. Mixing happens on the runtime side (guest-side mixing semantics:
ports submitting in the same frame window are summed into the same master
frames; volume is applied per port per channel).

```c
sceAudioOutOpen(userId, portType, index, lenFrames, freq, format);
sceAudioOutOutput(handle, ptr);   // submit exactly lenFrames of audio
sceAudioOutSetVolume(handle, chBitmask, volumes);  // per-channel 0..32768
sceAudioOutClose(handle);
```

* `userId` must be 0 (single-user); `portType`: 0 = Main, 1 = BGM, 2 = Voice;
  `index` must be 0; `freq` must be 48000 (the master domain is fixed —
  anything else is rejected, loudly, in the log); `format`: 0 = s16 mono,
  1 = s16 stereo, 2 = f32 stereo.
* Handles are 1-based; every documented failure returns -1 and logs why.
* Consumption model: without pacing, submits are consumed immediately.
  With a target FPS authorized (`psbet run --fps 60`), each presented frame
  authorizes exactly (48000/fps) master frames — audio consumption is glued
  to video pacing. A submit while the unplayed queue is at/over the latency
  cap blocks briefly (guest-cooperative); the measured queue depth is
  reported in the run summary (`max latency`).
* Dropped samples are an invariant: 0 (a port submitting late is the only
  drop path and is counted, never silent).

## 14. Pad / input (v1.3)

```c
scePadOpen(userId, type, index, param);        // all-zero for pad 1
scePadReadState(handle, &state);               // current state snapshot
scePadRead(handle, states, count, &returned);  // 1 snapshot (current)
scePadGetControllerInformation(handle, &info); // touchpad + connection
```

* `PsbPadState`: buttons (PSB_PAD_BTN_*), sticks/ly/rx/ry 0..255
  (center 128), analog l2/r2 0..255, accelerometer in g, gyroscope in deg/s,
  `connected`, and `timestampUs` — the guest time at which this state became
  valid. **End-to-end input latency = observation time − timestampUs**;
  the state is refreshed at the read call itself, so a poll observes any
  already-valid state at its very next poll — measured median 1.3 ms,
  independent of GPU/backend speed (PHASE_4_REPORT §5.1). The
  scripted-driver acceptance (`input_demo`) is runner-structural:
  the MEDIAN press latency must be < max(25 ms, achieved frame interval
  + 8.3 ms poll allowance) — exactly the 25 ms product acceptance on a
  host holding the 60 FPS cadence — and every press must be
  < max(150 ms, 3× frame).
* Without a driver (`null`), every pad reports `connected = 0` — a correct,
  testable state. With `--pad-script FILE`, a deterministic time-keyed
  driver feeds states (CI + reproducible bug reports). With SDL3 built in,
  real game controllers (DualSense included) map through SDL's gamepad API,
  gyro included.

## 15. Network (v1.3: stubbed, logged, no connectivity)

```c
sceNetInit(0);              // -> PSB_NET_ERR_UNAVAILABLE  (-0x4E02)
sceNetSocket(...);          // -> PSB_NET_ERR_UNAVAILABLE
sceNetConnect/Bind/Listen/Accept/Send/Recv/CloseSocket(...);
                            // -> PSB_NET_ERR_NOT_INITIALIZED (-0x4E01)
sceNetCtlGetState(&state);  // returns 0, state = 0 (disconnected, always)
```

PSBET grants guests NO network access. Every call is logged (rate-limited:
first 8 calls in full, then aggregated) and refused with the documented
code. Nothing opens sockets or touches the host network stack — verifiable
by audit. Guests should gate online features on `sceNetCtlGetState`.

## 16. Asynchronous GPU queues (v1.4 — Phase 5)

Submissions are **asynchronous with console semantics**:

```c
sceGnmSubmitCommandBuffers(...);  // returns when ACCEPTED, not executed
sceGnmSubmitCompute(count, ccbAddrs, ccbSizesInBytes);   // compute queue
sceGnmWaitQueueIdle(PSB_QUEUE_COMPUTE);                  // cross-queue fence
```

1. **Execution model.** A submit snapshots the command buffer into a bounded
   FIFO (in-flight cap 2) and returns; a dedicated GPU worker executes
   buffers one at a time in submit order. Results are identical to the
   synchronous path — only scheduling changes. `--gpu-sync` restores inline
   execution (debugging / A-B).
2. **Data ownership.** The device reads GPU-visible memory (inline payloads,
   GPU-VA vertices, fill regions) at EXECUTION time. A guest that rewrites
   submitted data must multi-buffer (one slot per in-flight frame: the
   in-flight cap is 2, so 3 slots are provably safe — see `cube_vm.c`) or
   fence with `sceGnmWaitQueueIdle`.
3. **Compute queue.** `sceGnmSubmitCompute` routes to a second, independent
   queue. The Phase-5 compute surface is `CmdFillMemory` (op 7): a
   data-parallel fill of a GPU-VA region (u32 pattern, len multiple of 4,
   <= 64 MiB). Compute submits are not display frames (no present, no
   frame counting). Real compute-shader translation is future roadmap work.
4. **Cross-queue ordering.** The two queues are independent. Graphics work
   that consumes compute results MUST fence first:
   `sceGnmWaitQueueIdle(PSB_QUEUE_COMPUTE)`. Mask bits: 1 = graphics,
   2 = compute, 3 = both. The wait is guest-cooperative and exit-aware.
5. **Frame-limit interplay.** `--frames N` still requests exit(124) after
   the Nth completed frame; with pipelining the guest may be up to 2 frames
   ahead when the flag fires, so the final count lands in [N, N+2]. A guest
   that exits naturally at/after the limit keeps its own exit code.
6. **Backpressure.** A submit blocks (guest-cooperative, exit-aware) while
   2 submissions are queued/executing. Blocked submits wake on process exit
   and on the frame limit — teardown can never hang on a submitter.

Windows: both new imports ship with MASM thunks (`thunk3`/`thunk1`) and are
machine-audited by `ci/audit_thunks.py`; `compute_fill` and `mt_submit` are
hard CI gates on both platforms.

## 17. Versioning

`psbGetRuntimeVersion()` packs `major.minor.patch`. ABI v1 will not break
backward compatibility; additions are additive. Compile against
`include/psbet/psb_guest_api.h`.
