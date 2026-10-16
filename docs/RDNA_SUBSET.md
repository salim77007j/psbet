# PSBET RDNA Subset v1 — Normative Specification

**Status:** Phase 2 deliverable. Implemented in `src/gpu/rdna.cpp` (single
source of truth: one table drives the assembler, the disassembler and the
RDNA→SPIR-V recompiler — they cannot drift apart).

## Clean-room provenance

This subset is an original, self-consistent reconstruction inspired by the
public GFX10-family instruction *shapes* (encoding prefix + opcode + operand
fields). No Sony or AMD code, headers, or tools were used, and no instructions
were copied bit-for-bit from any proprietary source. Validation against real
hardware captures is future work (Phase 3+, requires user-provided, legally
obtained shader dumps). Until then, PSBET's shader path is exercised by the
round-trip corpus (6,300+ checks), the recompiler corpus (510 programs), and
driver-level validation of the emitted SPIR-V.

## Word format

Every instruction is 32 bits, optionally followed by literal words.

    31        27 26        op_end   ...operand fields...        0
    [ENC: 5]     [OP: 4-8]      [SDST/VDST/ATTR/EN4/TARGET/SBASE]

| Format | ENC (5 bits) | OP width | OP shift | In-word operand fields |
|---|---|---|---|---|
| SOP2   | 0b10110 (22) | 5 | 22 | SDST[21:16] SSRC1[15:8] SSRC0[7:0] |
| SOPK   | 0b10001 (17) | 4 | 23 | SDST[22:16] SIMM16[15:0] |
| SOP1   | 0b11001 (25) | 5 | 22 | SDST[15:8] SSRC0[7:0] |
| SOPC   | 0b11000 (24) | 5 | 22 | SSRC1[15:8] SSRC0[7:0] |
| SOPP   | 0b10111 (23) | 5 | 22 | SIMM16[15:0] |
| VOP2   | 0b10000 (16) | 8 | 19 | VDST[18:11] |
| VOP1   | 0b01111 (15) | 8 | 19 | VDST[18:11] |
| VOP3   | 0b01110 (14) | 8 | 19 | VDST[18:11] |
| VOPC   | 0b01101 (13) | 8 | 19 | — (both sources are literals) |
| VINTRP | 0b01100 (12) | 3 | 24 | VDST[18:11] ATTR[10:6] CHAN[5:4] |
| EXP    | 0b01011 (11) | — | —  | EN4[18:15] TARGET[14:10] |
| SMEM   | 0b01010 (10) | 4 | 23 | SDST[18:12] SBASE[11:5] |
| FLAT   | 0b01001 (9)  | 3 | 24 | VDST[18:11] |
| MIMG   | 0b01000 (8)  | — | —  | VDST[18:11] |

## Selector space (scalar sources and ALL vector source literals)

| Range      | Meaning |
|---|---|
| 0..105     | SGPR `s0..s105` |
| 106        | VCC (subset: scalar compare-out) |
| 128..191   | inline unsigned constant `selector - 128` |
| 253        | SCC |
| 256..511   | VGPR `v0..v255` (vector sources only) |

Destinations: `SDST` fields address SGPRs directly; `VDST` fields address
VGPRs directly. Branch offsets (`SIMM16` on `s_branch`/`s_cbranch_*`) are
**signed offsets in 32-bit words** relative to the instruction after the
branch.

## Instruction set (55 opcodes)

- **SOP2:** `s_add_u32, s_add_i32, s_sub_u32, s_and_b32, s_or_b32, s_xor_b32,
  s_lshl_b32, s_lshr_b32, s_mul_i32`
- **SOPK:** `s_movk_i32` (zero-extended 16-bit immediate)
- **SOP1:** `s_mov_b32, s_not_b32`
- **SOPC:** `s_cmp_{eq,lt,gt}_{i32,u32}` → SCC
- **SOPP:** `s_nop, s_branch, s_cbranch_scc0, s_cbranch_scc1, s_endpgm,
  s_waitcnt` (waitcnt is accepted and treated as a fence point)
- **VOP2:** `v_add_f32, v_sub_f32, v_mul_f32, v_min_f32, v_max_f32, v_and_b32,
  v_or_b32, v_lshlrev_b32, v_lshrrev_b32`
- **VOP1:** `v_mov_b32, v_cvt_f32_u32, v_cvt_f32_i32, v_cvt_u32_f32, v_rcp_f32,
  v_sqrt_f32, v_fract_f32`
- **VOP3:** `v_mad_f32, v_fma_f32` (three literal selectors)
- **VOPC:** `v_cmp_{lt,gt,eq,neq}_f32` → SCC
- **VINTRP:** `v_interp_p1_f32, v_interp_p2_f32, v_interp_mov_f32` — the
  uniform stage-input interface: fragment stage = interpolated input load;
  vertex stage = vertex-buffer fetch (ATTR = location, CHAN = component)
- **EXP:** pixel export — `TARGET` 0..7 = MRT0..7 (SPIR-V output Location),
  `TARGET` 12 = position (`BuiltIn Position`); `EN4` gates the four channels
  (disabled channels export 0); the four source selectors follow as literals
- **SMEM:** `s_load_dword, s_load_dwordx2, s_store_dword` — scalar memory via
  the SSBO at descriptor set 0, binding 0 (`SBASE` is a 2-SGPR-aligned base
  index in the full ISA; the subset treats it as an opaque id)
- **FLAT:** `flat_load_dword, flat_store_dword` — vector memory via the same
  SSBO (address/data selectors are literal words; `VDST` is the destination on
  load and unused on store, where `LIT1` carries the data selector)
- **MIMG:** `image_sample` — combined image sampler at set 0, binding = the
  `LIT1` slot; coordinate selector in `LIT0` (x from the selector, y from
  selector+1); result `.x` is written to `VDST` (full swizzles come with the
  resource-model phase)

## Control-flow contract (validated, never silently mis-lowered)

The recompiler accepts exactly one structured shape per conditional:

    s_cbranch_sccX +N            ; cond ? jump : fallthrough
    <fallthrough body>           ; executed when cond == (X == 0)
    [s_branch +M]                ; optional shared merge jump (else shape)
    <else body>
    merge:                       ; further code, or s_endpgm

Anything else — backward jumps (loops), bare forward branches outside this
shape, cross-jumps into region bodies — is **rejected with an error**. Values
defined inside a region body are invalidated at the merge (documented
dominance contract: reads see 0; programs reload what they need).

## Recompiler output contract

- SPIR-V 1.0, Shader capability, GLSL.std.450, logical/GLSL450 memory model
- Entry point name `psb_main`; complete interface list (validated under
  strict drivers — recompiled modules pass `vkCreateShaderModule` on real
  implementations, verified against lavapipe)
- Deterministic: the same input produces byte-identical output (asserted by
  the corpus)

## Corpus

`psbet_rdna_tests` (CI-gated): 6,300+ round-trip checks (encode→decode field
equality across operand patterns for all 55 opcodes), 510 unique
recompile cases (deterministic + structurally asserted), positive/negative
control-flow tests, and malformed-input rejection tests. Exit code = failed
test count.
