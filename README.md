# PSBET

**PSBET** is an independent, clean-room runtime that executes PlayStation 5–class
software (x86-64 guest images with Orbis-style system interfaces) on standard
PCs running Linux and Windows.

> **Status: Phase 2 complete** — everything from Phase 1 (proven end-to-end on
> real homebrew guests, ASan/UBSan clean) plus: a **Vulkan GPU backend** with
> an RDNA→SPIR-V **shader recompiler** (table-driven, 55-opcode documented
> subset, 510-program validation corpus), shader/pipeline caches, a
> thread-correct process lifecycle (cooperative exit/termination), Windows
> guest execution with SEH crash diagnostics, and a signal-guarded Vulkan
> self-test that degrades gracefully on broken driver stacks. See
> [`docs/PHASE_2_REPORT.md`](docs/PHASE_2_REPORT.md) and
> [`docs/RDNA_SUBSET.md`](docs/RDNA_SUBSET.md).

![Cube demo](docs/screenshots/cube_soak_frame_5400.png)

*The `cube.elf` test homebrew — a rotating, depth-tested 3D scene — rendered
through PSBET's complete pipeline: guest ELF → dynamic loader → libkernel
shim → GPU command buffers → rasterizer → screenshot (1280×720).*

---

## Why this architecture

The PS5's CPU is x86-64 (AMD Zen 2) — the same instruction set as every
modern PC. PSBET therefore does **not** emulate a CPU: guest code runs at
native speed and every kernel/GPU operation is intercepted at the API boundary
(the same philosophy that made WINE and shadPS4 viable). Console-specific
behavior is reimplemented, cleanly, from public documentation:

| Subsystem     | Strategy                                                        | Phase 1 state        |
|---------------|------------------------------------------------------------------|----------------------|
| CPU (Zen 2)   | Native execution, imports bound at load time                     | ✅ Working           |
| Memory        | Guest address-space manager + direct-memory allocations          | ✅ Working           |
| Kernel/libc   | `sceKernel*` shim on host primitives (threads, TLS, sync, I/O)   | ✅ 33 functions      |
| GPU (RDNA 2)  | Command-buffer executor → software rasterizer (reference)        | ✅ Working           |
| GPU (RDNA 2)  | Vulkan backend (offscreen, loader-based)                         | ✅ Working           |
| GPU (RDNA 2)  | RDNA→SPIR-V shader recompiler (documented subset, validated)     | ✅ Working           |
| Filesystem    | PFS/PKG layer                                                    | 🔜 Phase 3           |
| Audio / Input | SDL-based                                                        | 🔜 Phase 4           |

## GPU backends

PSBET has two interchangeable GPU backends behind one command surface:

```bash
psbet gpu                                  # probe the host Vulkan stack
psbet run cube.elf --backend auto          # default: Vulkan if healthy, else software
psbet run cube.elf --backend vulkan        # force Vulkan (fails fast if broken)
psbet run cube.elf --backend software      # force the reference rasterizer
```

The Vulkan backend is fully offscreen (renders to Vulkan images, reads back to
the framebuffer) — no window or swapchain needed. A signal-guarded self-test
runs before every guest: if the host driver cannot render reliably, PSBET says
so and falls back to software instead of producing garbage. Set
`PSBET_SHADER_CACHE=<dir>` to persist translated shaders across runs.

## Quick start (Linux)

```bash
git clone https://github.com/salim77007j/psbet.git
cd psbet
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

# Run the bundled homebrew (no console files required):
./build/psbet run build/cube.elf --frames 90 \
    --screenshot-dir shots --screenshot-interval 30
```

Every push is built and tested by GitHub Actions on Linux (GCC, plus an
ASan/UBSan job) and Windows (MSVC); binaries and demo screenshots are
published as CI artifacts.

## Writing your own guest (homebrew)

PSBET executes standard ELF64 images built with the recipe in
[`docs/GUEST_ABI.md`](docs/GUEST_ABI.md):

```bash
gcc -shared -fPIC -nostartfiles -Wl,-e,psb_entry -o game.elf game.c
```

Guests link against nothing: undefined symbols are resolved by PSBET's
`sceKernel*` kernel shim and `sceGnm*` GPU submit interface at load time.

## What PSBET will NOT do

PSBET does not and will not ship or accept responsibility for console
proprietary material. See the legal notice below. Encrypted SELF containers
are detected and rejected with guidance; decrypting content you legally own
for interoperability is your responsibility under your local law.

## Legal framework — read this

- **Independent, original work.** PSBET is a clean-room implementation.
  It contains **zero** Sony code, keys, firmware, blobs, or copyrighted data.
  Interface names (`sceKernel*`, `sceGnm*`) are used solely for
  interoperability and familiarity; all semantics here are PSBET's own
  documented clean-room definitions.
- **No distribution of protected material.** PSBET never distributes BIOS,
  firmware, decryption keys, or games, and offers no assistance in
  circumventing copy protection.
- **User responsibility.** You are solely responsible for the legality of
  any files you run with PSBET under the laws of your jurisdiction.
- **Trademarks.** "PlayStation", "PS5" and related marks belong to Sony
  Interactive Entertainment; PSBET is not affiliated with, endorsed by, or
  connected to Sony in any way.

## Repository layout

```
include/psbet/       Guest-facing ABI header (psb_guest_api.h) + common types
src/core/            Memory manager, ELF64 loader, kernel shim, CPU supervisor
src/gpu/             Command stream (PBFF v1), software rasterizer, PNG writer
src/platform/        Virtual-memory primitives (mmap / VirtualAlloc)
src/tools/           The `psbet` CLI (run / info)
tests/               Unit tests + homebrew guests (hello.c, cube.c)
docs/                Architecture, roadmap, guest ABI, phase reports, evidence
.github/workflows/   CI: Linux + Windows + sanitizer builds, artifacts
```

## License

MIT — see [`LICENSE`](LICENSE). The clean-room policy in `LICENSE` is part
of the license grant conditions.
