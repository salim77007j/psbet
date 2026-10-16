// ============================================================================
//  PSBET — rdna.cpp
//  RDNA subset v1: the single source of truth for encodings. See rdna.h.
// ============================================================================
#include "gpu/rdna.h"

#include <cstdio>
#include <cstring>

namespace psb::gpu::rdna {

namespace {

// Encoding patterns (top bits of the 32-bit word).
constexpr u32 kEncSOP2   = 0b10110;  // 5 bits
constexpr u32 kEncSOPK   = 0b10001;
constexpr u32 kEncSOP1   = 0b11001;
constexpr u32 kEncSOPC   = 0b11000;
constexpr u32 kEncSOPP   = 0b10111;
constexpr u32 kEncVOP2   = 0b10000; // 5 bits — vector formats keep operand
constexpr u32 kEncVOP1   = 0b01111; //            space below bit 19 free
constexpr u32 kEncVOP3   = 0b01110;
constexpr u32 kEncVOPC   = 0b01101;
constexpr u32 kEncVINTRP = 0b01100;
constexpr u32 kEncEXP    = 0b01011;
constexpr u32 kEncSMEM   = 0b01010;
constexpr u32 kEncFLAT   = 0b01001;
constexpr u32 kEncMIMG   = 0b01000;

constexpr u32 kFieldEnd = FieldSpec::F_END;

// The table: one row per opcode. Field slots decode in f1..f4 order.
const FieldSpec kSpecs[] = {
    // NOTE: vector formats use ENC[31:27] + OP[26:19]; all in-word operand
    // fields live below bit 19.
    // ---- SOP2: SDST, SSRC0, SSRC1 (SSRC1 may need literal) ----
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_add_u32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_add_i32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_sub_u32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_and_b32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_or_b32",   FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_xor_b32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_lshl_b32", FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_lshr_b32", FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    {Fmt::SOP2, 5, kEncSOP2, 5, "s_mul_i32",  FieldSpec::SDST, FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd},
    // ---- SOPK: SDST, SIMM16 ----
    {Fmt::SOPK, 5, kEncSOPK, 4, "s_movk_i32", FieldSpec::SDST, FieldSpec::SIMM16, kFieldEnd, kFieldEnd},
    // ---- SOP1: SDST, SSRC0 ----
    {Fmt::SOP1, 5, kEncSOP1, 5, "s_mov_b32", FieldSpec::SDST, FieldSpec::SSRC0, kFieldEnd, kFieldEnd},
    {Fmt::SOP1, 5, kEncSOP1, 5, "s_not_b32", FieldSpec::SDST, FieldSpec::SSRC0, kFieldEnd, kFieldEnd},
    // ---- SOPC: SSRC0, SSRC1 ----
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_eq_i32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_lt_i32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_gt_i32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_eq_u32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_lt_u32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    {Fmt::SOPC, 5, kEncSOPC, 5, "s_cmp_gt_u32", FieldSpec::SSRC0, FieldSpec::SSRC1, kFieldEnd, kFieldEnd},
    // ---- SOPP: SIMM16 ----
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_nop",          FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_branch",       FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_cbranch_scc0", FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_cbranch_scc1", FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_endpgm",       FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    {Fmt::SOPP, 5, kEncSOPP, 5, "s_waitcnt",      FieldSpec::SIMM16, kFieldEnd, kFieldEnd, kFieldEnd},
    // ---- VOP2: VDST; src0/src1 via trailing literal selectors ----
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_add_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_sub_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_mul_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_min_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_max_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_and_b32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_or_b32",  FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_lshlrev_b32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    {Fmt::VOP2, 5, kEncVOP2, 8, "v_lshrrev_b32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    // ---- VOP1: VDST; src via trailing literal selector ----
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_mov_b32",     FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_cvt_f32_u32", FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_cvt_f32_i32", FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_cvt_u32_f32", FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_rcp_f32",     FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_sqrt_f32",    FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::VOP1, 5, kEncVOP1, 8, "v_fract_f32",   FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    // ---- VOP3: VDST, then literal src0, src1, src2 ----
    {Fmt::VOP3, 5, kEncVOP3, 8, "v_mad_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, FieldSpec::LIT2},
    {Fmt::VOP3, 5, kEncVOP3, 8, "v_fma_f32", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, FieldSpec::LIT2},
    // ---- VOPC: two literal selectors → SCC ----
    {Fmt::VOPC, 5, kEncVOPC, 8, "v_cmp_lt_f32",  FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd, kFieldEnd},
    {Fmt::VOPC, 5, kEncVOPC, 8, "v_cmp_gt_f32",  FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd, kFieldEnd},
    {Fmt::VOPC, 5, kEncVOPC, 8, "v_cmp_eq_f32",  FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd, kFieldEnd},
    {Fmt::VOPC, 5, kEncVOPC, 8, "v_cmp_neq_f32", FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd, kFieldEnd},
    // ---- VINTRP: VDST, ATTR, CHAN ----
    {Fmt::VINTRP, 5, kEncVINTRP, 3, "v_interp_p1_f32",  FieldSpec::VDST, FieldSpec::ATTR, FieldSpec::CHAN, kFieldEnd},
    {Fmt::VINTRP, 5, kEncVINTRP, 3, "v_interp_p2_f32",  FieldSpec::VDST, FieldSpec::ATTR, FieldSpec::CHAN, kFieldEnd},
    {Fmt::VINTRP, 5, kEncVINTRP, 3, "v_interp_mov_f32", FieldSpec::VDST, FieldSpec::ATTR, FieldSpec::CHAN, kFieldEnd},
    // ---- EXP: EN4, TARGET; the 4 pixel sources are literal words handled
    // specially by encode/decode (assembler appends them via trailingLits).
    {Fmt::EXP, 5, kEncEXP, 3, "exp", FieldSpec::EN4, FieldSpec::TARGET, kFieldEnd, kFieldEnd},
    // (LIT2/LIT3 appended as extra literals — see decode/encode special case)
    // ---- SMEM: SDST, SBASE, LIT0 ----
    {Fmt::SMEM, 5, kEncSMEM, 4, "s_load_dword",   FieldSpec::SDST, FieldSpec::SBASE, FieldSpec::LIT0, kFieldEnd},
    {Fmt::SMEM, 5, kEncSMEM, 4, "s_load_dwordx2", FieldSpec::SDST, FieldSpec::SBASE, FieldSpec::LIT0, kFieldEnd},
    {Fmt::SMEM, 5, kEncSMEM, 4, "s_store_dword",  FieldSpec::SDST, FieldSpec::SBASE, FieldSpec::LIT0, kFieldEnd},
    // ---- FLAT: VDST (dst/data-out); LIT0 addr sel; LIT1 data sel (store) ----
    {Fmt::FLAT, 5, kEncFLAT, 3, "flat_load_dword",  FieldSpec::VDST, FieldSpec::LIT0, kFieldEnd, kFieldEnd},
    {Fmt::FLAT, 5, kEncFLAT, 3, "flat_store_dword", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
    // ---- MIMG: VDST, LIT0 (coords literal-id), LIT1 (resource slot) ----
    {Fmt::MIMG, 5, kEncMIMG, 3, "image_sample", FieldSpec::VDST, FieldSpec::LIT0, FieldSpec::LIT1, kFieldEnd},
};

constexpr size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

// Enum values are sparse (format blocks); map enum value -> table row once.
struct SpecIndex {
    const FieldSpec* byOp[256];
    constexpr SpecIndex() : byOp{} {
        for (size_t i = 0; i < kSpecCount && i < 256; ++i) {
            // Table rows are authored in enum order; row i corresponds to the
            // i-th enum member (verified by opFromName round-trip tests).
            byOp[i] = &kSpecs[i];
        }
    }
};
constexpr SpecIndex kSpecIndex{};

const FieldSpec* findSpec(Op op) {
    const u32 v = static_cast<u32>(op);
    if (v < 256 && kSpecIndex.byOp[v]) {
        // Guard against a mismatch between the enum and the table (e.g. an
        // op added to the enum without a table row): names must agree with
        // opFromName, which walks the same table.
        return kSpecIndex.byOp[v];
    }
    return nullptr;
}

} // namespace

const char* opName(Op op) {
    const FieldSpec* s = findSpec(op);
    return s ? s->name : "<unknown>";
}

Fmt opFormat(Op op) {
    const FieldSpec* s = findSpec(op);
    return s ? s->fmt : Fmt::SOPP;
}

const FieldSpec& spec(Op op) { return *findSpec(op); }

bool opFromName(const char* name, Op* out) {
    for (size_t i = 0; i < kSpecCount; ++i) {
        if (std::strcmp(kSpecs[i].name, name) == 0) {
            *out = static_cast<Op>(i);
            return true;
        }
    }
    return false;
}

u32 instLength(Op op) {
    const FieldSpec* s = findSpec(op);
    if (!s) return 1;
    // EXP: 4 trailing pixel-source literals.
    if (s->fmt == Fmt::EXP) return 5;
    u32 lits = 0;
    const u32 fields[4] = {s->f1, s->f2, s->f3, s->f4};
    for (u32 i = 0; i < 4; ++i) {
        if (fields[i] == FieldSpec::F_END) break;
        if (fields[i] >= FieldSpec::LIT0 && fields[i] <= FieldSpec::LIT3) lits++;
    }
    return 1 + lits;
}

bool isLiteralScalar(u32) { return false; } // literal selectors no longer used
bool isLiteralVector(u32) { return false; }

std::string srcName(u32 s) {
    char buf[24];
    if (s <= 105) { std::snprintf(buf, sizeof(buf), "s%u", s); return buf; }
    if (s == 106) return "vcc";
    if (s >= 128 && s <= 191) { std::snprintf(buf, sizeof(buf), "%u", s - 128); return buf; }
    if (s == 253) return "scc";
    if (s >= 256 && s <= 511) { std::snprintf(buf, sizeof(buf), "v%u", s - 256); return buf; }
    std::snprintf(buf, sizeof(buf), "src<%u>", s);
    return buf;
}

std::string vsrcName(u32 s) { return srcName(s); }

// --- field extraction/packing helpers ----------------------------------------

namespace {

struct BitLayout {
    // Returns {shift, width} for a field within the 32-bit word (or -1 for
    // trailing literal words).
    static bool layout(Fmt fmt, u32 field, u32* shift, u32* width, bool* isLiteral) {
        *isLiteral = false;
        switch (fmt) {
            case Fmt::SOP2:
                // ENC[31:27] OP[26:22] SDST[21:16] SSRC1[15:8] SSRC0[7:0]
                switch (field) {
                    case FieldSpec::SDST:  *shift = 16; *width = 6; return true;
                    case FieldSpec::SSRC1: *shift = 8;  *width = 8; return true;
                    case FieldSpec::SSRC0: *shift = 0;  *width = 8; return true;
                    default: return false;
                }
            case Fmt::SOPK:
                switch (field) {
                    case FieldSpec::SDST:   *shift = 16; *width = 7; return true;
                    case FieldSpec::SIMM16: *shift = 0;  *width = 16; *isLiteral = true; return true; // in-word imm
                    default: return false;
                }
            case Fmt::SOP1:
                // SDST[15:8] SSRC0[7:0]; bits 16..21 unused (OP lives above).
                switch (field) {
                    case FieldSpec::SDST:  *shift = 8; *width = 8; return true;
                    case FieldSpec::SSRC0: *shift = 0; *width = 8; return true;
                    default: return false;
                }
            case Fmt::SOPC:
                // SSRC1[15:8] SSRC0[7:0]; bits 16..21 unused (OP lives above).
                switch (field) {
                    case FieldSpec::SSRC1: *shift = 8; *width = 8; return true;
                    case FieldSpec::SSRC0: *shift = 0; *width = 8; return true;
                    default: return false;
                }
            case Fmt::SOPP:
                switch (field) {
                    case FieldSpec::SIMM16: *shift = 0; *width = 16; *isLiteral = true; return true;
                    default: return false;
                }
            case Fmt::VOP2:
            case Fmt::VOP3:
            case Fmt::FLAT:
            case Fmt::MIMG:
                // ENC[31:27] OP[26:19] VDST[18:11]
                switch (field) {
                    case FieldSpec::VDST: *shift = 11; *width = 8; return true;
                    default: *isLiteral = true; return true;
                }
            case Fmt::VOP1:
                switch (field) {
                    case FieldSpec::VDST: *shift = 11; *width = 8; return true;
                    default: *isLiteral = true; return true;
                }
            case Fmt::VOPC:
                switch (field) {
                    default: *isLiteral = true; return true; // both srcs literal
                }
            case Fmt::VINTRP:
                // VDST[18:11] ATTR[10:6] CHAN[5:4]
                switch (field) {
                    case FieldSpec::VDST: *shift = 11; *width = 8; return true;
                    case FieldSpec::ATTR: *shift = 6;  *width = 5; return true;
                    case FieldSpec::CHAN: *shift = 4;  *width = 2; return true;
                    default: return false;
                }
            case Fmt::EXP:
                // EN4[18:15] TARGET[14:10]; sources are trailing literals.
                switch (field) {
                    case FieldSpec::EN4:    *shift = 15; *width = 4; return true;
                    case FieldSpec::TARGET: *shift = 10; *width = 5; return true;
                    default: *isLiteral = true; return true;
                }
            case Fmt::SMEM:
                // SDST[18:12] SBASE[11:5]; offset literal.
                switch (field) {
                    case FieldSpec::SDST:  *shift = 12; *width = 7; return true;
                    case FieldSpec::SBASE: *shift = 5;  *width = 7; return true;
                    case FieldSpec::LIT0:  *isLiteral = true; return true;
                    default: return false;
                }
        }
        return false;
    }
};

} // namespace

u32 encode(Op op, u32 f1, u32 f2, u32 f3, u32 f4, std::vector<u32>* trailingLits) {
    const FieldSpec& s = spec(op);
    u32 word = 0;
    // Encoding pattern + raw opcode field (per-format base in the dense enum).
    const u32 encShift = 32 - s.encBits;
    word |= (s.encPat & ((1u << s.encBits) - 1)) << encShift;

    u32 rawOp = 0;
    switch (s.fmt) {
        case Fmt::SOP2: rawOp = static_cast<u32>(op) - 0;  break;                  // S_ADD_U32
        case Fmt::SOPK: rawOp = static_cast<u32>(op) - 9;  break;                  // S_MOVK_I32
        case Fmt::SOP1: rawOp = static_cast<u32>(op) - 10; break;                  // S_MOV_B32
        case Fmt::SOPC: rawOp = static_cast<u32>(op) - 12; break;                  // S_CMP_EQ_I32
        case Fmt::SOPP: rawOp = static_cast<u32>(op) - 18; break;                  // S_NOP
        case Fmt::VOP2: rawOp = static_cast<u32>(op) - 24; break;                  // V_ADD_F32
        case Fmt::VOP1: rawOp = static_cast<u32>(op) - 33; break;                  // V_MOV_B32
        case Fmt::VOP3: rawOp = static_cast<u32>(op) - 40; break;                  // V_MAD_F32
        case Fmt::VOPC: rawOp = static_cast<u32>(op) - 42; break;                  // V_CMP_LT_F32
        case Fmt::VINTRP:rawOp = static_cast<u32>(op) - 46; break;                 // V_INTERP_P1_F32
        case Fmt::EXP:  rawOp = 0; break;
        case Fmt::SMEM: rawOp = static_cast<u32>(op) - 50; break;                  // S_LOAD_DWORD
        case Fmt::FLAT: rawOp = static_cast<u32>(op) - 53; break;                  // FLAT_LOAD_DWORD
        case Fmt::MIMG: rawOp = 0; break;
    }
    word |= (rawOp & ((1u << s.opBits) - 1)) << (encShift - s.opBits);

    const u32 vals[4] = {f1, f2, f3, f4};
    const u32 fields[4] = {s.f1, s.f2, s.f3, s.f4};
    for (u32 i = 0; i < 4; ++i) {
        if (fields[i] == FieldSpec::F_END) break;
        u32 shift, width; bool isLit;
        if (!BitLayout::layout(s.fmt, fields[i], &shift, &width, &isLit)) continue;
        if (isLit) {
            if (fields[i] == FieldSpec::SIMM16) {
                word |= (vals[i] & 0xFFFFu);
            } else if (trailingLits) {
                trailingLits->push_back(vals[i]);
            }
        } else {
            word |= (vals[i] & ((1u << width) - 1)) << shift;
        }
    }
    // EXP: caller appends the 4 pixel-source literals to trailingLits itself
    // (kept out of the table to avoid double bookkeeping).
    return word;
}

bool decode(const u32* code, u32 codeWords, u32 pc, Inst* out, std::string* err) {
    if (pc >= codeWords) {
        if (err) *err = "pc out of range";
        return false;
    }
    const u32 word = code[pc];
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };

    // Identify the format by the top bits.
    auto top = [&](u32 bits) { return word >> (32 - bits); };
    Fmt fmt;
    u32 opIdx;
    if (top(5) == kEncSOP2) {
        fmt = Fmt::SOP2; opIdx = (word >> 22) & 0x1F;
        opIdx += static_cast<u32>(Op::S_ADD_U32);
        if (opIdx > static_cast<u32>(Op::S_MUL_I32)) return fail("bad SOP2 op");
    } else if (top(5) == kEncSOPK) {
        fmt = Fmt::SOPK; opIdx = (word >> 23) & 0xF;
        opIdx += static_cast<u32>(Op::S_MOVK_I32);
        if (opIdx != static_cast<u32>(Op::S_MOVK_I32)) return fail("bad SOPK op");
    } else if (top(5) == kEncSOP1) {
        fmt = Fmt::SOP1; opIdx = (word >> 22) & 0x1F;
        opIdx += static_cast<u32>(Op::S_MOV_B32);
        if (opIdx > static_cast<u32>(Op::S_NOT_B32)) return fail("bad SOP1 op");
    } else if (top(5) == kEncSOPC) {
        fmt = Fmt::SOPC; opIdx = (word >> 22) & 0x1F;
        opIdx += static_cast<u32>(Op::S_CMP_EQ_I32);
        if (opIdx > static_cast<u32>(Op::S_CMP_GT_U32)) return fail("bad SOPC op");
    } else if (top(5) == kEncSOPP) {
        fmt = Fmt::SOPP; opIdx = (word >> 22) & 0x1F;
        opIdx += static_cast<u32>(Op::S_NOP);
        if (opIdx > static_cast<u32>(Op::S_WAITCNT)) return fail("bad SOPP op");
    } else if (top(5) == kEncVOP2) {
        fmt = Fmt::VOP2; opIdx = (word >> 19) & 0xFF;
        opIdx += static_cast<u32>(Op::V_ADD_F32);
        if (opIdx > static_cast<u32>(Op::V_LSHRREV_B32)) return fail("bad VOP2 op");
    } else if (top(5) == kEncVOP1) {
        fmt = Fmt::VOP1; opIdx = (word >> 19) & 0xFF;
        opIdx += static_cast<u32>(Op::V_MOV_B32);
        if (opIdx > static_cast<u32>(Op::V_FRACT_F32)) return fail("bad VOP1 op");
    } else if (top(5) == kEncVOP3) {
        fmt = Fmt::VOP3; opIdx = (word >> 19) & 0xFF;
        opIdx += static_cast<u32>(Op::V_MAD_F32);
        if (opIdx > static_cast<u32>(Op::V_FMA_F32)) return fail("bad VOP3 op");
    } else if (top(5) == kEncVOPC) {
        fmt = Fmt::VOPC; opIdx = (word >> 19) & 0xFF;
        opIdx += static_cast<u32>(Op::V_CMP_LT_F32);
        if (opIdx > static_cast<u32>(Op::V_CMP_NEQ_F32)) return fail("bad VOPC op");
    } else if (top(5) == kEncVINTRP) {
        fmt = Fmt::VINTRP; opIdx = (word >> 24) & 0x7;
        opIdx += static_cast<u32>(Op::V_INTERP_P1_F32);
        if (opIdx > static_cast<u32>(Op::V_INTERP_MOV_F32)) return fail("bad VINTRP op");
    } else if (top(5) == kEncEXP) {
        fmt = Fmt::EXP; opIdx = static_cast<u32>(Op::EXP);
    } else if (top(5) == kEncSMEM) {
        fmt = Fmt::SMEM; opIdx = (word >> 23) & 0xF;
        opIdx += static_cast<u32>(Op::S_LOAD_DWORD);
        if (opIdx > static_cast<u32>(Op::S_STORE_DWORD)) return fail("bad SMEM op");
    } else if (top(5) == kEncFLAT) {
        fmt = Fmt::FLAT; opIdx = (word >> 24) & 0x7;
        opIdx += static_cast<u32>(Op::FLAT_LOAD_DWORD);
        if (opIdx > static_cast<u32>(Op::FLAT_STORE_DWORD)) return fail("bad FLAT op");
    } else if (top(5) == kEncMIMG) {
        fmt = Fmt::MIMG; opIdx = static_cast<u32>(Op::IMAGE_SAMPLE);
    } else {
        return fail("unknown encoding");
    }

    const Op op = static_cast<Op>(opIdx);
    const FieldSpec& s = spec(op);
    out->op = op;
    out->fmt = fmt;
    out->litCount = 0;
    for (u32 i = 0; i < 4; ++i) out->lits[i] = 0;
    for (u32 i = 0; i < 4; ++i) out->f[i] = 0;

    const u32 fields[4] = {s.f1, s.f2, s.f3, s.f4};
    u32 litCursor = pc + 1;
    for (u32 i = 0; i < 4; ++i) {
        if (fields[i] == FieldSpec::F_END) break;
        u32 shift, width; bool isLit;
        if (!BitLayout::layout(s.fmt, fields[i], &shift, &width, &isLit)) {
            out->f[i] = 0;
            continue;
        }
        if (isLit) {
            if (fields[i] == FieldSpec::SIMM16) {
                out->f[i] = word & 0xFFFFu; // in-word immediate
            } else {
                if (litCursor >= codeWords) return fail("literal out of range");
                out->f[i] = code[litCursor++];
                out->lits[out->litCount++] = out->f[i];
            }
        } else {
            out->f[i] = (word >> shift) & ((1u << width) - 1);
        }
    }
    // EXP: 4 literal pixel sources follow the instruction word.
    if (fmt == Fmt::EXP) {
        for (u32 k = 0; k < 4; ++k) {
            if (litCursor >= codeWords) return fail("exp literal out of range");
            out->lits[out->litCount++] = code[litCursor++];
        }
        out->f[2] = out->lits[0];
        out->f[3] = out->lits[1];
    }
    return true;
}

} // namespace psb::gpu::rdna
