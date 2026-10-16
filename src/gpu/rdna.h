// ============================================================================
//  PSBET — rdna.h
//  RDNA (GFX10.3-class) shader ISA subset: table-driven encoder/decoder.
//
//  CLEAN-ROOM NOTE
//  ---------------
//  This module implements PSBET's normative "RDNA subset v1": the encodings
//  below are an original, self-consistent reconstruction of the public
//  GFX10-family instruction shapes (encoding-prefix + opcode + operand
//  fields). No Sony or AMD code, headers or tools were used. Bit layout is
//  defined by the shared table in rdna.cpp — the assembler, the disassembler
//  and the recompiler all derive from that one table, so they cannot drift
//  apart. Validation against real hardware captures is future work (requires
//  user-provided, legally obtained shader dumps).
//
//  Operand encodings — selector space (used by scalar source fields and by
//  every vector source literal):
//    0..105   SGPR s0..s105
//    106      VCC (subset: scalar carry/compare-out)
//    128..191 inline unsigned constant (value = selector - 128)
//    253      SCC
//    256..511 VGPR v0..v255 (valid only where a vector source is documented)
//  Destinations: SDST fields use the SGPR space, VDST fields the VGPR space.
//  Branch offsets (SIMM16) are SIGNED offsets in 32-bit words relative to
//  the instruction after the branch.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "psbet/common.h"

namespace psb::gpu::rdna {

// ---- instruction opcodes (subset; values are DENSE and match the encoding
// table rows one-to-one — see rdna.cpp kSpecs) --------------------------------
enum class Op : u32 {
    // SOP2 — scalar, two sources + scalar dest (raw opcode field 0..8)
    S_ADD_U32 = 0,
    S_ADD_I32 = 1,
    S_SUB_U32 = 2,
    S_AND_B32 = 3,
    S_OR_B32  = 4,
    S_XOR_B32 = 5,
    S_LSHL_B32 = 6,
    S_LSHR_B32 = 7,
    S_MUL_I32 = 8,

    // SOPK — scalar, 16-bit immediate (raw 0)
    S_MOVK_I32 = 9,

    // SOP1 — scalar, one source (raw 0..1)
    S_MOV_B32 = 10,
    S_NOT_B32 = 11,

    // SOPC — scalar compare (writes SCC) (raw 0..5)
    S_CMP_EQ_I32 = 12,
    S_CMP_LT_I32 = 13,
    S_CMP_GT_I32 = 14,
    S_CMP_EQ_U32 = 15,
    S_CMP_LT_U32 = 16,
    S_CMP_GT_U32 = 17,

    // SOPP — scalar program flow (raw 0..5)
    S_NOP      = 18,
    S_BRANCH   = 19,
    S_CBRANCH_SCC0 = 20,
    S_CBRANCH_SCC1 = 21,
    S_ENDPGM   = 22,
    S_WAITCNT  = 23,

    // VOP2 — vector, two sources (raw 0..8)
    V_ADD_F32 = 24,
    V_SUB_F32 = 25,
    V_MUL_F32 = 26,
    V_MIN_F32 = 27,
    V_MAX_F32 = 28,
    V_AND_B32 = 29,
    V_OR_B32  = 30,
    V_LSHLREV_B32 = 31,
    V_LSHRREV_B32 = 32,

    // VOP1 — vector, one source (raw 0..6)
    V_MOV_B32 = 33,
    V_CVT_F32_U32 = 34,
    V_CVT_F32_I32 = 35,
    V_CVT_U32_F32 = 36,
    V_RCP_F32 = 37,
    V_SQRT_F32 = 38,
    V_FRACT_F32 = 39,

    // VOP3 — vector, three sources (raw 0..1)
    V_MAD_F32 = 40,
    V_FMA_F32 = 41,

    // VOPC — vector compare (raw 0..3)
    V_CMP_LT_F32 = 42,
    V_CMP_GT_F32 = 43,
    V_CMP_EQ_F32 = 44,
    V_CMP_NEQ_F32 = 45,

    // VINTRP — stage input fetch / interpolation (raw 0..2)
    V_INTERP_P1_F32 = 46,
    V_INTERP_P2_F32 = 47,
    V_INTERP_MOV_F32 = 48,

    // EXP — pixel export (4 literal sources)
    EXP = 49,

    // SMEM — scalar memory (raw 0..2)
    S_LOAD_DWORD   = 50,
    S_LOAD_DWORDX2 = 51,
    S_STORE_DWORD  = 52,

    // FLAT — vector memory (raw 0..1)
    FLAT_LOAD_DWORD  = 53,
    FLAT_STORE_DWORD = 54,

    // MIMG — image sample (raw 0)
    IMAGE_SAMPLE = 55,
};

enum class Fmt : u32 {
    SOP2 = 0,
    SOPK,
    SOP1,
    SOPC,
    SOPP,
    VOP2,
    VOP1,
    VOP3,
    VOPC,
    VINTRP,
    EXP,
    SMEM,
    FLAT,
    MIMG,
};

// Which fields an instruction has (order matters; documented subset).
struct FieldSpec {
    Fmt fmt;
    u32   encBits;   // encoding pattern width
    u32   encPat;    // pattern value (left-aligned in the top encBits bits)
    u32   opBits;
    const char* name;
    // Operand field slots, in decode order. Terminated by F_END.
    enum F : u32 {
        F_END = 0,
        SDST, SSRC0, SSRC1,      // scalar
        VDST, VSRC1,             // vector (9-bit)
        SIMM16, LIT0, LIT1, LIT2, LIT3,  // immediates / literal words
        ATTR, CHAN,              // VINTRP
        EN4, TARGET,             // EXP
        SBASE,                   // SMEM base pair
    };
    u32 f1, f2, f3, f4;
};

// Scalar/vector register name helpers.
std::string srcName(u32 s);         // scalar operand space
std::string vsrcName(u32 s);        // vector operand space
bool isLiteralScalar(u32 s);        // needs the trailing literal word
bool isLiteralVector(u32 s);

// Decoded instruction.
struct Inst {
    Op     op = Op::S_NOP;
    Fmt    fmt = Fmt::SOPP;
    u32    f[4] = {0, 0, 0, 0};  // field values (per FieldSpec order)
    u32    lits[4] = {0, 0, 0, 0}; // literal words consumed
    u32    litCount = 0;
};

// Decode one instruction from `code` at `pc` (32-bit words). Returns false on
// an unknown encoding/opcode (with `err` set) — never silently skips bytes.
bool decode(const u32* code, u32 codeWords, u32 pc, Inst* out, std::string* err);

// Encode (assembler core; also used to build the test corpus).
u32 encode(Op op, u32 f1, u32 f2, u32 f3, u32 f4, std::vector<u32>* trailingLits);

// Instruction length in words (including trailing literals).
u32 instLength(Op op);

const char* opName(Op op);
bool opFromName(const char* name, Op* out);
Fmt opFormat(Op op);
const FieldSpec& spec(Op op);

} // namespace psb::gpu::rdna
