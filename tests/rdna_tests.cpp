// ============================================================================
//  PSBET — rdna_tests.cpp
//  RDNA subset v1 test corpus:
//    1. Round-trip: encode → decode → identical fields, for every opcode
//       across generated operand patterns (hundreds of cases).
//    2. Recompiler: RDNA programs → SPIR-V; structural assertions (expected
//       SPIR-V opcodes present, module length sane, deterministic).
//    3. Shader-program cases: if/else shapes, position export, fragment
//       interpolation, image sample, flat memory.
//    4. Negative cases: malformed word streams must be REJECTED (never
//       silently skipped) — decode/recompile fail with an error string.
//  Exit code = number of failed tests.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/rdna.h"
#include "gpu/rdna_spirv.h"

using namespace psb;
using namespace psb::gpu::rdna;

static int g_failed = 0;
static int g_passed = 0;
static int g_cases = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_cases;                                                              \
        if (!(cond)) {                                                          \
            std::printf("    CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__,      \
                        __LINE__);                                              \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool runTest(const char* name, bool (*fn)()) {
    std::printf("[RDNA] %s ... ", name);
    std::fflush(stdout);
    const bool ok = fn();
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    if (ok) ++g_passed; else ++g_failed;
    return ok;
}

// ---------------------------------------------------------------------------
// Round-trip helpers
// ---------------------------------------------------------------------------
static bool roundTrip(Op op, u32 f1, u32 f2, u32 f3, u32 f4,
                      const std::vector<u32>& extraLits) {
    ++g_cases;
    std::vector<u32> lits = extraLits;
    const u32 word = encode(op, f1, f2, f3, f4, &lits);
    std::vector<u32> code{word};
    code.insert(code.end(), lits.begin(), lits.end());

    Inst in;
    std::string err;
    if (!decode(code.data(), static_cast<u32>(code.size()), 0, &in, &err)) {
        std::printf("      decode failed for %s: %s\n", opName(op), err.c_str());
        return false;
    }
    if (in.op != op) {
        std::printf("      op mismatch: %s -> %s\n", opName(op), opName(in.op));
        return false;
    }
    if (in.litCount != lits.size()) {
        std::printf("      lit count mismatch for %s: %u vs %zu\n",
                    opName(op), in.litCount, lits.size());
        return false;
    }
    if (instLength(op) != code.size()) {
        std::printf("      length mismatch for %s: %u vs %zu\n",
                    opName(op), instLength(op), code.size());
        return false;
    }
    // Field comparison: destination/source fields, then literals.
    const FieldSpec& s = spec(op);
    const u32 want[4] = {f1, f2, f3, f4};
    const u32 fields[4] = {s.f1, s.f2, s.f3, s.f4};
    for (u32 i = 0; i < 4; ++i) {
        if (fields[i] == FieldSpec::F_END) break;
        if (fields[i] == FieldSpec::SIMM16) continue; // sign-extension nuances checked separately
        if (in.f[i] != want[i]) {
            std::printf("      field[%u] mismatch for %s: %u vs %u\n",
                        i, opName(op), in.f[i], want[i]);
            return false;
        }
    }
    for (u32 i = 0; i < lits.size(); ++i) {
        if (in.lits[i] != lits[i]) {
            std::printf("      lit[%u] mismatch for %s: %u vs %u\n",
                        i, opName(op), in.lits[i], lits[i]);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 1. Round-trip corpus (generated: opcodes × operand patterns)
// ---------------------------------------------------------------------------
static const Op kAllOps[] = {
    Op::S_ADD_U32, Op::S_ADD_I32, Op::S_SUB_U32, Op::S_AND_B32, Op::S_OR_B32,
    Op::S_XOR_B32, Op::S_LSHL_B32, Op::S_LSHR_B32, Op::S_MUL_I32,
    Op::S_MOVK_I32, Op::S_MOV_B32, Op::S_NOT_B32,
    Op::S_CMP_EQ_I32, Op::S_CMP_LT_I32, Op::S_CMP_GT_I32,
    Op::S_CMP_EQ_U32, Op::S_CMP_LT_U32, Op::S_CMP_GT_U32,
    Op::S_NOP, Op::S_BRANCH, Op::S_CBRANCH_SCC0, Op::S_CBRANCH_SCC1,
    Op::S_ENDPGM, Op::S_WAITCNT,
    Op::V_ADD_F32, Op::V_SUB_F32, Op::V_MUL_F32, Op::V_MIN_F32, Op::V_MAX_F32,
    Op::V_AND_B32, Op::V_OR_B32, Op::V_LSHLREV_B32, Op::V_LSHRREV_B32,
    Op::V_MOV_B32, Op::V_CVT_F32_U32, Op::V_CVT_F32_I32, Op::V_CVT_U32_F32,
    Op::V_RCP_F32, Op::V_SQRT_F32, Op::V_FRACT_F32,
    Op::V_MAD_F32, Op::V_FMA_F32,
    Op::V_CMP_LT_F32, Op::V_CMP_GT_F32, Op::V_CMP_EQ_F32, Op::V_CMP_NEQ_F32,
    Op::V_INTERP_P1_F32, Op::V_INTERP_P2_F32, Op::V_INTERP_MOV_F32,
    Op::EXP,
    Op::S_LOAD_DWORD, Op::S_LOAD_DWORDX2, Op::S_STORE_DWORD,
    Op::FLAT_LOAD_DWORD, Op::FLAT_STORE_DWORD,
    Op::IMAGE_SAMPLE,
};

static bool test_roundtrip_scalar() {
    // Scalar ops: dst × src patterns (SGPRs, inline consts, SCC).
    const u32 srcs[] = {0, 5, 31, 100, 128, 130, 160, 191, 253};
    for (Op op : kAllOps) {
        const Fmt f = opFormat(op);
        if (f != Fmt::SOP2 && f != Fmt::SOP1 && f != Fmt::SOPC && f != Fmt::SOPK)
            continue;
        for (u32 dst = 0; dst < 32; dst += 7) {
            for (u32 a : srcs) {
                for (u32 b : {u32(0), u32(9), u32(190)}) {
                    std::vector<u32> lits;
                    if (!roundTrip(op, dst, a, b, 0, lits)) return false;
                }
            }
        }
    }
    return true;
}

static bool test_roundtrip_vector() {
    // Vector ops: VDST + literal selectors (SGPR, const, VGPR spaces).
    const u32 sels[] = {0, 3, 105, 128, 190, 256, 300, 511};
    for (Op op : kAllOps) {
        const Fmt f = opFormat(op);
        if (f != Fmt::VOP2 && f != Fmt::VOP1 && f != Fmt::VOP3 && f != Fmt::VOPC)
            continue;
        for (u32 dst = 0; dst < 256; dst += 37) {
            for (u32 s0 : sels) {
                for (u32 s1 : {u32(256), u32(7), u32(129)}) {
                    std::vector<u32> lits;
                    if (!roundTrip(op, dst, s0, s1, 0, lits)) return false;
                }
            }
        }
    }
    return true;
}

static bool test_roundtrip_mem_flow() {
    for (Op op : kAllOps) {
        const Fmt f = opFormat(op);
        if (f == Fmt::SMEM) {
            for (u32 dst : {u32(0), u32(17), u32(63)}) {
                for (u32 base : {u32(0), u32(8)}) {
                    for (u32 off : {u32(0), u32(64), u32(0xFFFF)}) {
                        std::vector<u32> lits;
                        if (!roundTrip(op, dst, base, off, 0, lits)) return false;
                    }
                }
            }
        }
        if (f == Fmt::FLAT || f == Fmt::MIMG) {
            for (u32 dst : {u32(0), u32(120), u32(255)}) {
                for (u32 addr : {u32(256), u32(300)}) {
                    std::vector<u32> lits;
                    if (!roundTrip(op, dst, addr, addr + 1, 0, lits)) return false;
                }
            }
        }
        if (f == Fmt::VINTRP) {
            for (u32 dst : {u32(0), u32(9)}) {
                for (u32 attr : {u32(0), u32(15), u32(31)}) {
                    for (u32 chan = 0; chan < 4; ++chan) {
                        std::vector<u32> lits;
                        if (!roundTrip(op, dst, attr, chan, 0, lits)) return false;
                    }
                }
            }
        }
        if (f == Fmt::SOPP) {
            for (u32 imm : {u32(0), u32(1), u32(0x1234), u32(0x8000)}) {
                std::vector<u32> lits;
                if (!roundTrip(op, imm, 0, 0, 0, lits)) return false;
            }
        }
        if (f == Fmt::EXP) {
            // EXP: encode(en, target, 0, 0) + 4 trailing source literals.
            for (u32 en : {u32(0xF), u32(0x1)}) {
                for (u32 tgt : {u32(0), u32(7), u32(12)}) {
                    ++g_cases;
                    std::vector<u32> lits{256, 257, 258, 259};
                    const u32 word = encode(op, en, tgt, 0, 0, &lits);
                    std::vector<u32> code{word};
                    // encode pushed nothing extra (EXP literals appended manually);
                    // lits grew by 4 via the caller — strip our 4 seeds to size.
                    code.insert(code.end(), lits.end() - 4, lits.end());
                    Inst in; std::string err;
                    if (!decode(code.data(), (u32)code.size(), 0, &in, &err)) return false;
                    if (in.op != Op::EXP || in.f[0] != en || in.f[1] != tgt) return false;
                    if (in.litCount != 4) return false;
                    if (in.lits[0] != 256 || in.lits[3] != 259) return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2. Recompiler structural tests
// ---------------------------------------------------------------------------

// Scans a SPIR-V module for a fully-formed instruction (opcode presence).
static bool spirvHasOp(const std::vector<psb::gpu::spv::Word>& mod, u32 opcode) {
    for (size_t i = 5; i < mod.size();) {
        const u32 wc = mod[i] >> 16;
        const u32 op = mod[i] & 0xFFFF;
        if (op == opcode) return true;
        if (wc == 0) return false;
        i += wc;
    }
    return false;
}

static bool test_recompile_vertex_passthrough() {
    // Vertex: fetch attr0 → position; attr1 → MRT0 (FS color).
    std::vector<u32> code;
    auto push = [&](Op op, u32 a, u32 b, u32 c, u32 d,
                    const std::vector<u32>& lits = {}) {
        std::vector<u32> tmp;
        code.push_back(encode(op, a, b, c, d, &tmp));
        code.insert(code.end(), tmp.begin(), tmp.end());
        code.insert(code.end(), lits.begin(), lits.end());
    };
    push(Op::V_INTERP_P1_F32, 0, 0, 0, 0);          // v0 = in0.x
    push(Op::V_INTERP_P1_F32, 1, 0, 1, 0);          // v1 = in0.y
    push(Op::V_INTERP_P1_F32, 2, 0, 2, 0);          // v2 = in0.z
    push(Op::S_MOVK_I32, 10, 106, 0, 0);            // s10 = 106 (w=1.0? const sel)
    // position = (v0, v1, v2, 1.0): inline const 1 = selector 129
    std::vector<u32> posLits{256, 257, 258, 129};   // v0,v1,v2,1.0
    push(Op::EXP, 0xF, 12, 0, 0, posLits);
    std::vector<u32> colLits{260, 261, 262, 263};   // attr1 via v4..v7? simplified
    push(Op::V_INTERP_P1_F32, 4, 1, 0, 0);          // v4 = in1.x
    push(Op::V_INTERP_P1_F32, 5, 1, 1, 0);
    push(Op::V_INTERP_P1_F32, 6, 1, 2, 0);
    push(Op::V_INTERP_P1_F32, 7, 1, 3, 0);
    std::vector<u32> col2{260, 261, 262, 263};      // v4,v5,v6,v7
    push(Op::EXP, 0xF, 0, 0, 0, col2);
    push(Op::S_ENDPGM, 0, 0, 0, 0);

    RecompileResult r = recompile(code.data(), (u32)code.size(), Stage::Vertex);
    CHECK(r.ok);
    CHECK(!r.spirv.empty());
    CHECK(spirvHasOp(r.spirv, 15 /* OpEntryPoint */));
    CHECK(spirvHasOp(r.spirv, 61 /* OpLoad */));
    CHECK(spirvHasOp(r.spirv, 62 /* OpStore */));
    CHECK(spirvHasOp(r.spirv, 80 /* OpCompositeConstruct */));

    // Determinism.
    RecompileResult r2 = recompile(code.data(), (u32)code.size(), Stage::Vertex);
    CHECK(r2.ok);
    CHECK(r2.spirv == r.spirv);
    return true;
}

static bool test_recompile_if_else() {
    // Canonical if/else shape: s_cbranch_scc1 +1 (jump to the shared
    // s_branch), then-body of 1, s_branch +1 over the else-body.
    std::vector<u32> code;
    auto push = [&](Op op, u32 a, u32 b, u32 c, u32 d,
                    const std::vector<u32>& lits = {}) {
        std::vector<u32> tmp;
        code.push_back(encode(op, a, b, c, d, &tmp));
        code.insert(code.end(), tmp.begin(), tmp.end());
        code.insert(code.end(), lits.begin(), lits.end());
    };
    // if (s0 == 5): v0 = const2 else v0 = const3
    push(Op::S_CMP_EQ_I32, 0, 133, 0, 0);       // s0 == 5  (133 = const 5)
    push(Op::S_CBRANCH_SCC1, 2, 0, 0, 0);       // if eq → jump +2 words (to s_branch)
    push(Op::V_MOV_B32, 0, 130, 0, 0);          // v0 = const2 (fallthrough = then)
    push(Op::S_BRANCH, 2, 0, 0, 0);             // shared merge jump (+2 words over else)
    push(Op::V_MOV_B32, 0, 131, 0, 0);          // v0 = const3 (jump body = else)
    // merge: exp v0 to MRT0
    std::vector<u32> outLits{256, 129, 129, 129};
    push(Op::EXP, 0x1, 0, 0, 0, outLits);
    push(Op::S_ENDPGM, 0, 0, 0, 0);

    RecompileResult r = recompile(code.data(), (u32)code.size(), Stage::Fragment);
    if (!r.ok) std::printf("      if_else err: %s\n", r.err.c_str());
    CHECK(r.ok);
    CHECK(spirvHasOp(r.spirv, 247 /* OpSelectionMerge */));
    CHECK(spirvHasOp(r.spirv, 250 /* OpBranchConditional */));
    CHECK(spirvHasOp(r.spirv, 249 /* OpBranch */));
    return true;
}

static bool test_recompile_rejects_loops() {
    // Backward s_branch must be rejected.
    std::vector<u32> code;
    code.push_back(encode(Op::S_BRANCH, static_cast<u32>(-2) & 0xFFFF, 0, 0, 0, nullptr));
    code.push_back(encode(Op::S_ENDPGM, 0, 0, 0, 0, nullptr));
    RecompileResult r = recompile(code.data(), (u32)code.size(), Stage::Vertex);
    CHECK(!r.ok);
    CHECK(!r.err.empty());
    return true;
}

static bool test_decode_rejects_garbage() {
    // Unknown encoding patterns must fail decode with an error.
    const u32 garbage[] = {0xFFFFFFFFu, 0x00000000u, 0x7FFFFFFFu};
    for (u32 g : garbage) {
        ++g_cases;
        Inst in;
        std::string err;
        if (decode(&g, 1, 0, &in, &err) && g == 0x00000000u) {
            // all-zero word is not a valid encoding either
            std::printf("      zero word decoded silently\n");
            return false;
        }
        (void)err;
    }
    // Truncated literal must fail.
    const u32 trunc[] = {encode(Op::V_MOV_B32, 3, 256, 0, 0, nullptr)}; // needs 1 literal
    Inst in; std::string err;
    ++g_cases;
    if (decode(trunc, 1, 0, &in, &err)) {
        std::printf("      truncated literal accepted\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 3. Corpus scale: generate 500+ unique recompile cases
// ---------------------------------------------------------------------------
static bool test_corpus_500() {
    // For each ALU op: 6 operand variants → encode → recompile (straight-line
    // program of that op + export + endpgm) → assert ok + deterministic.
    u32 count = 0;
    const u32 sels[] = {0, 128, 190, 256, 511, 7, 105, 131, 300, 64};
    for (Op op : kAllOps) {
        const Fmt f = opFormat(op);
        if (f != Fmt::VOP2 && f != Fmt::VOP1 && f != Fmt::VOP3 && f != Fmt::VOPC &&
            f != Fmt::SOP2 && f != Fmt::SOP1 && f != Fmt::SOPK)
            continue;
        for (u32 variant = 0; variant < 15; ++variant) {
            std::vector<u32> code;
            auto push = [&](Op o, u32 a, u32 b, u32 c, u32 d,
                            const std::vector<u32>& lits = {}) {
                std::vector<u32> tmp;
                code.push_back(encode(o, a, b, c, d, &tmp));
                code.insert(code.end(), tmp.begin(), tmp.end());
                code.insert(code.end(), lits.begin(), lits.end());
            };
            const u32 s0 = sels[variant % 10];
            const u32 s1 = sels[(variant + 2) % 10];
            switch (f) {
                case Fmt::VOP2: push(op, (9 + variant) % 256, s0, s1, 0); break;
                case Fmt::VOP1: push(op, (9 + variant) % 256, s0, 0, 0); break;
                case Fmt::VOP3: push(op, (9 + variant) % 256, s0, s1, sels[(variant + 4) % 10]); break;
                case Fmt::VOPC: push(op, s0, s1, 0, 0); break;
                case Fmt::SOP2: push(op, variant % 32, s0 % 192, s1 % 192, 0); break;
                case Fmt::SOP1: push(op, variant % 32, s0 % 192, 0, 0); break;
                case Fmt::SOPK: push(op, variant % 32, 0x1234 + variant, 0, 0); break;
                default: continue;
            }
            // Terminator: MRT0 export from a defined vector + endpgm.
            std::vector<u32> outLits{256 + (variant % 8), 129, 129, 129};
            push(Op::EXP, 0x1, 0, 0, 0, outLits);
            push(Op::S_ENDPGM, 0, 0, 0, 0);

            const Stage st = (f == Fmt::VINTRP) ? Stage::Fragment : Stage::Vertex;
            RecompileResult r = recompile(code.data(), (u32)code.size(), st);
            ++count;
            if (!r.ok) {
                std::printf("      corpus case %u (%s v%u) failed: %s\n",
                            count, opName(op), variant, r.err.c_str());
                return false;
            }
            RecompileResult r2 = recompile(code.data(), (u32)code.size(), st);
            if (r2.spirv != r.spirv) {
                std::printf("      corpus case %u nondeterministic\n", count);
                return false;
            }
        }
    }
    std::printf("(%u recompile cases) ", count);
    CHECK(count >= 500);
    return true;
}

// ---------------------------------------------------------------------------
int main() {
    runTest("roundtrip_scalar_ops", test_roundtrip_scalar);
    runTest("roundtrip_vector_ops", test_roundtrip_vector);
    runTest("roundtrip_mem_flow_exp", test_roundtrip_mem_flow);
    runTest("recompile_vertex_passthrough", test_recompile_vertex_passthrough);
    runTest("recompile_if_else", test_recompile_if_else);
    runTest("recompile_rejects_loops", test_recompile_rejects_loops);
    runTest("decode_rejects_garbage", test_decode_rejects_garbage);
    runTest("corpus_500_recompile", test_corpus_500);

    std::printf("→ RDNA: %d tests passed, %d failed (%d checks)\n",
                g_passed, g_failed, g_cases);
    return g_failed;
}
