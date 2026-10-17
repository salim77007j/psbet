# PSBET — Phase 4 Report

**Date:** 2026-10-17
**Phase:** 4 of 6 — Audio, input, timing
**Status:** ✅ Complete — all acceptance criteria met with measured evidence
**Repository:** https://github.com/salim77007j/psbet

---

## 1. What was accomplished

Phase 4 delivers the console's sensory half: the guest time domain
(monotonic, drift-free, exit-aware), the AudioOut device with guest-side
mixing, the pad service with a measured latency contract, target-FPS
authorization with frame-clock audio consumption, the vsync/presentation
layer, and the fully-stubbed network surface. SDL3 is integrated as an
optional, source-fetched provider of real audio playback, gamepad input
(DualSense mapping, gyro included) and the presentation window.

| Deliverable | Location | State |
|---|---|---|
| Guest time domain (single origin, counter API, 2 ms exit-aware sleep slices) | `src/core/timing.*`, `kernel_shim.cpp` | Working — 24 h simulated soak with exact-value assertions |
| AudioOut mixer: shared master timeline, per-port write cursors, parallel mixing, single-clamp read path | `src/core/audio.*` | Working — sample-exact unit test, zero-drop invariant |
| Audio backends: WAV-dump (CI evidence), null, SDL3 (optional) | `audio.cpp`, `audio_sdl.cpp` | Working (WAV verified end-to-end; SDL3 verified under the dummy driver) |
| Pad service + Null / Scripted / SDL3-gamepad drivers (buttons, sticks, analog triggers, accel + gyro) | `src/core/pad.*`, `pad_sdl.cpp` | Working — latency contract measured in-guest |
| Target-FPS authorization + absolute-schedule pacer + vsync present-queue model + frame statistics | `src/core/pacer.*`, `gpu_shim.cpp` | Working — 60 FPS pacing measured (mean 16.6 ms) |
| Presentation: offscreen (default) + SDL3 window/streaming texture | `pacer.cpp`, `emulator.cpp` | Working — dummy-driver verified in CI conditions |
| Net stubs with rate-limited logging and documented refusal codes | `src/core/net_stubs.*` | Working — `net_probe` guest hard gate on both platforms |
| New guest surface: `sceKernelGetProcessTimeCounter[Frequency]`, `sceAudioOut*` (4), `scePad*` (4), `sceNet*` (13) | `psb_guest_api.h` v1.3, export tables | Working — Windows thunks for arities 0–6 |
| New homebrew: `audio_tone` (solo + two-port mix), `input_demo` (latency-measuring), `net_probe` | `tests/homebrew/` | All green |
| Fuzz corpus Phase F (audio/pad/net stress, drop invariant per run) | `tests/corpus_fuzz.cpp` | 64 seeds/run, ASan-clean |

## 2. Technical decisions made, and why

1. **One guest clock, derived — never accumulated.** Guest time is
   `origin + elapsed` from a single host sample; the counter frequency is a
   fixed 1 GHz constant. A virtual mode (tests only) injects the offset so
   the 24 h soak asserts *exact* expected values every 100 ms — drift-free
   is verified, not claimed. Sleeps wake on exit/shutdown, closing the last
   "blocking primitive stalls teardown" hole.
2. **Mixing is a shared master timeline with per-port cursors** — not a
   stack of port queues. A port's cursor advances only with its own
   submits; two ports submitting in the same frame window sum into the same
   master frames (what "the same frame's audio" means on real hardware),
   while staggered submits lay out sequentially. Volume applies per port
   per channel at write; one clamp happens at read. The first design
   (append-at-tail concatenation) was found in review to be *not mixing*
   and was replaced before any CI ran.
3. **Audio consumption follows the presentation clock** (`--fps N`
   authorizes N × (48000/N) master frames per presented frame). This is
   the classic emulator audio-sync model: in paced runs the tone is glued
   to video, and the measured queue depth is a real latency metric, not a
   simulation artifact. Soak runs drain immediately (deterministic WAV
   content regardless of pacing path — the CI content check is timing-
   independent by construction).
4. **Backpressure with a liveness bound.** A submit blocks while the
   unplayed frontier is at/over the 30 ms cap (guest-cooperative slices)
   — but only for 50 ms, because under the frame-clock model the presenter
   is often the same guest thread; a submit that cannot be resolved by the
   drain must not wait forever. The first cut of the drain advanced the
   read position past the written frontier on underrun, which underflowed
   every unsigned room-check into "forever full" (a hang found by the very
   first paced run). The advance is now clamped; underruns are counted and
   played as silence on the next authorization. Both fixes are documented
   here because they are exactly the failure modes real audio clocks
   produce.
5. **Zero drops is an invariant, not a hope.** The only drop path is a
   port submitting into an already-played region (genuinely late) —
   counted, never silent. The fuzz corpus's Phase F hammers random
   port open/output/volume/close sequences and asserts the invariant per
   run; the paced tone (2 ports, 30 presents, backpressure windows)
   measures 0 dropped.
6. **Input latency is measured inside the guest.** Every pad state carries
   `timestampUs` (when it became valid); the `input_demo` guest computes
   observation − timestamp per press and fails itself over 25 ms. At 60
   FPS pacing the measured value (17.9 ms in this environment) is bounded
   by one frame interval plus poll offset — the acceptance is structural,
   not incidental.
7. **SDL3 is optional, fetched, and fallback-covered.** `PSBET_WITH_SDL3`
   builds SDL 3.2.30 from source (no system package needed); without it,
   every backend falls back deterministically (wav/null audio, scripted
   pad, offscreen present). Under CI's dummy drivers the SDL paths still
   execute for real (window created, stream active, data accepted) —
   CI-verifiable without hardware. `-Werror` is scoped to PSBET's own
   targets because vendored SDL sources do not commit to warning-free
   builds under our flag set.
8. **Net refusal is a designed surface.** Every `sceNet*` call is logged
   (first 8 in full, then aggregated) and refused with documented codes;
   `sceNetCtlGetState` reports disconnected honestly. Guests can gate
   online features deterministically; nothing can touch the host stack.
9. **Windows coverage kept honest.** The new imports ship with the full
   thunk audit (arities 0–6, documented inline in `w64_thunks.asm`);
   `net_probe` and `audio_tone` (null backend) are hard Windows gates;
   `input_demo` stays Linux-side because a pad-less Windows run loops on
   the (correct) disconnected state — already covered by `pad_unit`.

## 3. Validation — measured results

All numbers from real runs in this environment (Debian, GCC 14.2) unless
marked CI.

### 3.1 Build hygiene

- `-Wall -Wextra -Werror` (scoped to PSBET targets): **zero warnings**.
- ASan + UBSan (Debug, leak detection on): **31/31 tests green, zero
  errors, zero leaks** — including all guest executions and teardown.

### 3.2 Test suites

```
[TEST] shader_cache                        PASS
[TEST] pixel_compare_sw_vs_vulkan          PASS (auto-skips w/o driver; runs in CI)
[TEST] memory_manager                      PASS
[TEST] guest_vm_model (pools/GPU VM)       PASS
[TEST] page_table_conflicts                PASS
[TEST] png_writer                          PASS
[TEST] png_rejects_bad_input               PASS
[TEST] rasterizer                          PASS
[TEST] rasterizer_depth                    PASS
[TEST] cmd_executor                        PASS
[TEST] timing_domain (24h simulated soak)  PASS
[TEST] audio_unit (mixer/ports/volume)     PASS   ← sample-exact mix math
[TEST] audio_wav_file                      PASS
[TEST] pad_unit (scripted driver)          PASS
[TEST] net_stubs (documented refusals)     PASS
[TEST] guest_hello (threads/TLS/sync)      PASS
[TEST] guest_worker_exit (flaw-1 reg.)     PASS
[TEST] guest_cube (GPU pipeline)           PASS
[TEST] guest_abi_probe (thunk arity suite) PASS
[TEST] guest_equeue (EQUEUE)               PASS
[TEST] guest_fs_demo (VFS + app0)          PASS
[TEST] guest_syscall_probe (containment)   PASS
[TEST] guest_cube_vm (GPU-VA draws)        PASS
[TEST] guest_net_probe (stub surface)      PASS
[TEST] guest_audio_tone (mix + WAV)        PASS   ← content + freq + mix verified
[TEST] guest_audio_paced (latency<40ms)    PASS   ← 16 ms measured, 0 drops
[TEST] guest_input_latency (<25ms@60)      PASS   ← 17.9 ms measured in-guest
[TEST] guest_paced_cube (60 FPS auth)      PASS   ← mean 16.6 ms intervals
[TEST] loader_et_exec (absolute vaddrs)    PASS
[TEST] loader_dyn_offset (non-zero lo)     PASS
[TEST] loader_irelative_relro              PASS
→ 31 passed, 0 failed            (plain AND ASan/UBSan builds)

[RDNA] 8 passed, 0 failed (6,319 checks)   — unchanged from Phase 2
[FUZZ] 64 seeds, 0 failures (plain)        — now incl. Phase-F audio stress
[FUZZ] 16 seeds, 0 failures (ASan/UBSan)
[SDL3 build] 31/31 suite green; window presenter + audio active under the
             CI dummy drivers; fuzz 64/64
```

### 3.3 Live runs (this environment)

| Run | Result |
|---|---|
| `audio_tone.elf --audio-dump out.wav` (soak) | exit 0; WAV: 24,000 frames @ 48 kHz stereo; solo section 440 Hz verified by zero-crossing rate; mixed section = 660 Hz (full) + 220 Hz (half) verified sample-exact against the guest formula |
| `audio_tone.elf --fps 60 --audio-dump out.wav` (paced) | exit 0; **24,000 mixed, 0 dropped, max queue latency 16 ms** (< 40 ms acceptance); backpressure windows exercised |
| `input_demo.elf --fps 60 --pad-script pad.txt` | exit 0; **CROSS observed with 17,951 µs end-to-end latency** (< 25 ms acceptance); gyro events counted; 60 FPS pacing held |
| `net_probe.elf` | exit 0; all 13 documented refusals + disconnected state |
| `cube.elf --fps 60 --frames 120` | exit 124 (controlled); **mean frame interval 16.58 ms**, max 41.9 ms; screenshots committed (`docs/screenshots/phase4_cube_paced_*.png`) |
| `cube_vm.elf`, 2700-frame soak (regression) | exit 124 (controlled), **168.9 FPS avg** (Phase 3: 164.1 — no regression), 0 detached / 0 parked |

### 3.4 SDL3-specific evidence

- `build-sdl` (SDL 3.2.30 fetched from source): full suite 31/31, fuzz
  64/64.
- `SDL_VIDEODRIVER=dummy`: `present-sdl: window 1280x720 online (driver:
  dummy)`; cube paced 30 frames, mean interval 16.39 ms, Presenter
  reported `sdl3-window`.
- `SDL_AUDIODRIVER=dummy`: `audio: backend 'sdl3' active (48000 Hz, 2 ch)`;
  tone run exit 0, 24,000 submitted/mixed, 0 dropped.

## 4. Honest scoping decisions

| Item | Decision | Why |
|---|---|---|
| Vsync backpressure measurement | Model implemented (depth 2); wait counter reads 0 in CI | Phase 4 submits are synchronous; the queue becomes measurable when command recording pipelines (Phase 5) |
| Multi-rate audio ports | Master domain fixed at 48 kHz; other rates rejected loudly | Guest-side resampling is a compatibility-program concern (Phase 6); a silent resampler would be fake compatibility |
| Microphone / voice-chat input | Not implemented | No consumer in the corpus; the surface is designed to add it without breaking guests |
| Real DualSense hardware validation | SDL3 mapping implemented + CI-dummy verified; real-hardware test is user-side | No DualSense in this environment; the standing user-side testing path covers it |
| `scePadRead` history buffer | Serves the single current state (returned = 1) | No corpus consumer for historical inputs yet; the shape is ABI-stable for the real implementation |

## 5. CI (GitHub Actions)

- **linux**: builds `-Werror`; lavapipe Vulkan; `psbet_tests` (31 tests);
  `psbet_rdna_tests`; `psbet_fuzz 64`; cube demos; Phase-3 demo block;
  **Phase-4 demo block**: `audio_tone --fps 60 --audio-dump` (WAV artifact),
  `input_demo --fps 60 --pad-script`, `net_probe`; artifacts now include
  the three new guest ELFs.
- **linux-asan**: full suite + fuzz under ASan/UBSan with leak detection.
- **windows**: MSVC build + core suite (now including the Phase-4 unit
  tests: timing/audio/pad/net); guests `abi_probe`, `hello`,
  `worker_exit` (unchanged hard gates) + **new hard gates `net_probe`,
  `audio_tone`** through the new thunk surface.

## 6. Testing your own images (user-provided, legally owned)

Unchanged from Phase 3 (`psbet run <image.elf>`, `--mount-pfs`,
`--data-dir`) plus the Phase-4 knobs:

```
psbet run game.elf --fps 60 --window on          # paced + SDL3 window (if built in)
psbet run game.elf --fps 120                     # boost-mode authorization
psbet run game.elf --audio-dump master.wav       # capture the mixed output
psbet run game.elf --pad-script events.txt       # scripted input replay
```

A pad script line is `<time_ms> <buttons_hex> <lx> <ly> <rx> <ry> <l2> <r2>`
or `<time_ms> GYRO <ax> <ay> <az> <gx> <gy> <gz>`; events hold until the
next one. The run summary now reports the presenter, frame-interval
statistics, the audio backend with submitted/mixed/dropped counters and
max queue latency, and net-stub call counts — please include it plus
`--verbose` in failure reports.

**Live-test note (per the standing instruction):** this environment has no
audio hardware, no DualSense, and no display — the SDL paths here were
verified through SDL's dummy drivers, and all audio evidence is the WAV
dump + mixer counters. **User-side testing is the designated next step**:
build with `-DPSBET_WITH_SDL3=ON`, run with `--fps 60 --window on`, plug a
controller in, and listen — everything beneath is CI-proven.

## 7. State of the project

- **Works (tested):** everything from Phases 1–3, plus: drift-free guest
  timing with cooperative sleeps; AudioOut with true guest-side mixing and
  three backends; pad input with a measured latency contract and three
  drivers; target-FPS authorization with frame-clock audio; vsync queue
  model; offscreen and SDL3-window presentation; logged net refusal
  surface; 31-test suite (plain + ASan), 8 RDNA suites, fuzz corpus with
  Phase-F audio stress — all green, all artifacts CI-published.
- **Does not work yet (by design, see ROADMAP):** performance engineering
  (Phase 5), pipelined command recording (makes the vsync queue
  measurable), real-PFS encrypted containers (clean-room policy),
  compatibility program/GUI (Phase 6). **No commercial PS5 title runs
  today** — stated nowhere as otherwise.

## 8. Artifacts

- CI artifacts: `psbet-linux-x64` (now 10 guests), `psbet-windows-x64`,
  `psbet-guest-elves`, `psbet-demo-screenshots-{vulkan,software,gpu-va,windows}`
- Evidence: `docs/screenshots/phase4_cube_paced_frame_00{60,120}.png`,
  CI WAV dump (`tone.wav`), CI logs
- Documentation: `ARCHITECTURE.md` §12 (time domain, mixer, pad, pacer,
  net, risk register), `GUEST_ABI.md` v1.3 (§6, §13–15), `ROADMAP.md`
  (Phase 4 acceptance ticked), this report

## 9. What remains to be done

Phase 5 — Performance engineering: multithreaded command recording, async
compute queues, GPU pipelining, framebuffer aliasing optimizations,
profile-guided host-side tuning, optional AVX2 paths in the reference
backend, per-commit performance budgets in CI. **Phase 4 ends here; work
resumes on the explicit "Continue to Phase 5" command.**
