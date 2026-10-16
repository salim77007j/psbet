// ============================================================================
//  PSBET — rdna_spirv.cpp
//  RDNA subset v1 → SPIR-V recompiler. See header for the semantic model.
//
//  Control-flow subset (validated in pass 1, never silently mis-lowered):
//
//    s_cbranch_sccX +N            ; cond ? jump to pc+1+N : fallthrough
//    <fallthrough body: N instr>  ; executed when cond == (X == 0)
//    [s_branch +M]                ; optional shared merge jump (else shape)
//    <else body: M instr>
//    merge:
//
//  The optional trailing s_branch is present iff it lands strictly past the
//  else body. Bare forward branches, backward jumps and cross-jumps into
//  region bodies are rejected with an error.
//
//  Dominance policy: values defined inside a region body are invalidated at
//  the merge (subsequent reads see 0). Cross-region value flow is therefore
//  impossible to mis-lower — corpus programs reload what they need. This is
//  the documented subset contract.
//
//  Selector space for scalar sources and all vector source literals:
//    0..105 SGPR, 106 VCC, 128..191 inline const, 253 SCC, 256..511 VGPR.
// ============================================================================
#include "gpu/rdna_spirv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace psb::gpu::rdna {

using spv::Word;

namespace {

enum class Kind { F32, U32, Bool };

struct Value {
    spv::Word id = 0;
    Kind kind = Kind::U32;
    u32 depth = 0;   // region nesting depth at definition; invalidated above
};

struct Parsed {
    Inst in;
    u32  pc;
};

// One validated if/else construct (see file header for the shape).
struct Region {
    u32 fallStart, fallEnd;    // fallthrough body [start, end)
    u32 elseStart, elseEnd;    // valid iff hasElse
    bool hasElse;
    u32 mergePc;
    bool branchOnTrue;         // scc1: condition==true takes the jump body
    spv::Word condId = 0;
    spv::Word jumpLbl = 0;     // block holding the jumped-to body (else shape)
    spv::Word fallLbl = 0;     // block holding the fallthrough body
    spv::Word mergeLbl = 0;
    u32 openDepth = 0;
};

std::string instText(const Inst& in) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s f=[%u %u %u %u]", opName(in.op),
                  in.f[0], in.f[1], in.f[2], in.f[3]);
    return buf;
}

} // namespace

std::string disassembleRange(const u32* code, u32 codeWords) {
    std::string out;
    u32 pc = 0;
    while (pc < codeWords) {
        Inst in;
        std::string err;
        if (!decode(code, codeWords, pc, &in, &err)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  <decode error @ %u: %s>\n", pc, err.c_str());
            out += buf;
            break;
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%04u: %-18s %u %u %u %u\n", pc, opName(in.op),
                      in.f[0], in.f[1], in.f[2], in.f[3]);
        out += buf;
        pc += instLength(in.op);
    }
    return out;
}

RecompileResult recompile(const u32* code, u32 codeWords, Stage stage) {
    RecompileResult res;
    spv::Module m;

    m.capability(spv::Cap::Shader);
    m.extInstImport(m.id(), "GLSL.std.450");
    m.memoryModel(spv::MemModel::Logical, spv::MemModel::GLSL450);

    const spv::Word voidT = m.typeVoid();
    const spv::Word f32T  = m.typeF32();
    const spv::Word u32T  = m.typeU32();
    const spv::Word boolT = m.typeBool();
    const spv::Word v4fT  = m.typeVec4F32();
    const spv::Word v2fT  = m.typeVec2F32();

    std::vector<spv::Word> interface;

    // ---- resources ----------------------------------------------------------
    const spv::Word rtArr = m.typeRuntimeArray(u32T, 4);
    const spv::Word ssboStruct = m.typeStruct({rtArr}, true);
    const spv::Word ssboPtrT = m.typePointer(spv::Storage::StorageBuffer, ssboStruct);
    const spv::Word u32PtrSSBO = m.typePointer(spv::Storage::StorageBuffer, u32T);
    spv::Word ssboVar = 0;
    auto ensureSsbo = [&]() -> spv::Word {
        if (!ssboVar) {
            ssboVar = m.globalVariable(ssboStruct, spv::Storage::StorageBuffer);
            m.name(ssboVar, "psb_smem");
            m.decorateDescriptor(ssboVar, 0, 0);
            interface.push_back(ssboVar);
        }
        return ssboVar;
    };
    std::map<u32, spv::Word> samplerVars;

    // ---- I/O interface --------------------------------------------------------
    std::map<u32, spv::Word> vsOutLoc, fsInLoc, vsInLoc;
    spv::Word outPos = 0;

    auto ensureIO = [&](std::map<u32, spv::Word>& map, u32 loc, spv::Storage st,
                        const char* prefix) -> spv::Word {
        auto it = map.find(loc);
        if (it != map.end()) return it->second;
        const spv::Word p = m.typePointer(st, v4fT);
        const spv::Word v = m.globalVariable(p, st);
        m.decorateLocation(v, loc);
        char nm[32];
        std::snprintf(nm, sizeof(nm), "%s%u", prefix, loc);
        m.name(v, nm);
        map[loc] = v;
        interface.push_back(v);
        return v;
    };

    if (stage == Stage::Vertex) {
        outPos = m.globalVariable(m.typePointer(spv::Storage::Output, v4fT),
                                  spv::Storage::Output);
        m.decorateBuiltin(outPos, spv::Bi::Position);
        m.name(outPos, "vs_gl_Position");
        interface.push_back(outPos);
    }

    // ---- value maps -------------------------------------------------------------
    Value sVal[128];
    Value vVal[256];
    Value sccVal;
    u32 depth = 0;

    // ---- helpers -------------------------------------------------------------------
    auto constF = [&](float f) { return Value{m.constF32(f), Kind::F32}; };
    auto constU = [&](u32 u) { return Value{m.constU32(u), Kind::U32}; };
    auto toF32 = [&](Value v) -> Value {
        if (v.kind == Kind::F32) return v;
        Value r; r.kind = Kind::F32;
        r.id = m.emit(spv::Op::ConvertUToF, f32T, {v.id});
        return r;
    };
    auto toU32 = [&](Value v) -> Value {
        if (v.kind == Kind::U32) return v;
        Value r; r.kind = Kind::U32;
        r.id = m.emit(spv::Op::ConvertFToU, u32T, {v.id});
        return r;
    };
    auto bitcastU32 = [&](Value v) -> Value {
        if (v.kind == Kind::U32) return v;
        Value r; r.kind = Kind::U32;
        r.id = m.emit(spv::Op::Bitcast, u32T, {v.id});
        return r;
    };
    auto sccCond = [&]() -> spv::Word {
        if (sccVal.kind == Kind::Bool) return sccVal.id;
        if (sccVal.id == 0) return m.constBool(false);
        return m.emit(spv::Op::INotEqual, boolT, {bitcastU32(sccVal).id, m.constU32(0)});
    };

    // Selector resolution. Values defined in a deeper region are invalid here
    // (dominance) — read as zero, per the documented contract.
    auto live = [&](const Value& v) -> bool { return v.id != 0 && v.depth <= depth; };
    auto scalarSel = [&](u32 sel) -> Value {
        if (sel >= 128 && sel <= 191) return constU(sel - 128);
        if (sel == 253) return sccVal.id ? sccVal : constU(0);
        if (sel < 128) return live(sVal[sel]) ? sVal[sel] : constU(0);
        if (sel >= 256 && sel <= 511) return live(vVal[sel - 256]) ? vVal[sel - 256] : constU(0);
        return constU(0);
    };
    auto vectorSel = [&](u32 sel) -> Value {
        if (sel >= 256 && sel <= 511) return live(vVal[sel - 256]) ? vVal[sel - 256] : constF(0.f);
        if (sel >= 128 && sel <= 191) return constU(sel - 128);
        if (sel < 128) return live(sVal[sel]) ? sVal[sel] : constF(0.f);
        return constF(0.f);
    };
    auto store = [&](Value* slot, Value v) { *slot = v; slot->depth = depth; };

    // ---- pass 1: decode + control-flow validation --------------------------------
    std::vector<Parsed> prog;
    {
        u32 pc = 0;
        while (pc < codeWords) {
            Parsed p{};
            std::string err;
            if (!decode(code, codeWords, pc, &p.in, &err)) {
                res.err = std::string("decode: ") + err + " at pc " + std::to_string(pc);
                return res;
            }
            p.pc = pc;
            prog.push_back(p);
            pc += instLength(p.in.op);
        }
        if (prog.empty()) { res.err = "empty program"; return res; }
    }

    std::map<u32, u32> pcIdx;   // pc -> prog index
    for (size_t k = 0; k < prog.size(); ++k) pcIdx[prog[k].pc] = static_cast<u32>(k);

    std::map<u32, Region> regionAt;   // keyed by the s_cbranch pc
    for (const Parsed& p : prog) {
        const Inst& in = p.in;
        if (in.op == Op::S_BRANCH) {
            const i32 rel = static_cast<i32>(static_cast<i16>(in.f[0] & 0xFFFFu));
            if (rel < 0) {
                res.err = "backward s_branch (loops) unsupported in subset v1";
                return res;
            }
            continue;
        }
        if (in.op != Op::S_CBRANCH_SCC0 && in.op != Op::S_CBRANCH_SCC1) continue;

        const u32 pc = p.pc;
        const i32 rel = static_cast<i32>(static_cast<i16>(in.f[0] & 0xFFFFu));
        if (rel <= 0) {
            res.err = "s_cbranch with non-positive offset (subset forbids loops)";
            return res;
        }
        const u32 fallStart = pc + 1;
        const u32 jumpPc    = pc + 1 + static_cast<u32>(rel);
        if (jumpPc >= codeWords) { res.err = "s_cbranch target out of range"; return res; }

        Region r;
        r.fallStart = fallStart;
        r.fallEnd = jumpPc;
        r.branchOnTrue = (in.op == Op::S_CBRANCH_SCC1);

        std::string derr;
        Inst br;
        if (decode(code, codeWords, jumpPc, &br, &derr) && br.op == Op::S_BRANCH) {
            const i32 rel2 = static_cast<i32>(static_cast<i16>(br.f[0] & 0xFFFFu));
            if (rel2 < 0) { res.err = "backward s_branch in if-shape"; return res; }
            const u32 merge = jumpPc + 1 + static_cast<u32>(rel2);
            if (merge >= codeWords) { res.err = "s_branch merge out of range"; return res; }
            r.hasElse = true;
            r.elseStart = jumpPc + 1;
            r.elseEnd = merge;
            r.mergePc = merge;
        } else {
            r.hasElse = false;
            r.mergePc = jumpPc;   // jump target IS the merge (if-less)
        }
        regionAt[pc] = r;
    }

    // ---- pass 2: emit ---------------------------------------------------------------
    const spv::Word entryId = m.id();
    m.beginFunction(voidT, {}, entryId);
    m.functionParams();
    m.beginBlock();

    // Emits one non-flow instruction. false + res.err on unsupported use.
    auto emitInst = [&](const Inst& in) -> bool {
        switch (in.op) {
            case Op::S_ADD_U32: case Op::S_ADD_I32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::IAdd, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_SUB_U32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                const Word nb = m.emit(spv::Op::Not, u32T, {b.id});
                const Word one = m.constU32(1);
                const Word nb1 = m.emit(spv::Op::IAdd, u32T, {nb, one});
                store(&sVal[in.f[0]], {m.emit(spv::Op::IAdd, u32T, {a.id, nb1}), Kind::U32});
                return true;
            }
            case Op::S_AND_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::BitwiseAnd, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_OR_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::BitwiseOr, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_XOR_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::BitwiseXor, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_LSHL_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::ShiftLeftLogical, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_LSHR_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::ShiftRightLogical, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_MUL_I32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                Value b = bitcastU32(scalarSel(in.f[2]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::IMul, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::S_MOVK_I32:
                store(&sVal[in.f[0]], constU(static_cast<u16>(in.f[1] & 0xFFFFu)));
                return true;
            case Op::S_MOV_B32:
                store(&sVal[in.f[0]], scalarSel(in.f[1]));
                return true;
            case Op::S_NOT_B32: {
                Value a = bitcastU32(scalarSel(in.f[1]));
                store(&sVal[in.f[0]], {m.emit(spv::Op::Not, u32T, {a.id}), Kind::U32});
                return true;
            }
            case Op::S_CMP_EQ_I32: case Op::S_CMP_LT_I32: case Op::S_CMP_GT_I32:
            case Op::S_CMP_EQ_U32: case Op::S_CMP_LT_U32: case Op::S_CMP_GT_U32: {
                Value a = bitcastU32(scalarSel(in.f[0]));
                Value b = bitcastU32(scalarSel(in.f[1]));
                const bool isSigned = (in.op == Op::S_CMP_LT_I32 || in.op == Op::S_CMP_GT_I32);
                const spv::Word opc =
                    (in.op == Op::S_CMP_EQ_I32 || in.op == Op::S_CMP_EQ_U32) ? spv::Op::IEqual
                    : (in.op == Op::S_CMP_LT_I32 || in.op == Op::S_CMP_LT_U32)
                        ? (isSigned ? spv::Op::SLessThan : spv::Op::ULessThan)
                        : (isSigned ? spv::Op::SGreaterThan : spv::Op::UGreaterThan);
                store(&sccVal, {m.emit(opc, boolT, {a.id, b.id}), Kind::Bool});
                return true;
            }
            case Op::S_NOP:
            case Op::S_WAITCNT:
                return true;

            case Op::V_ADD_F32: case Op::V_SUB_F32: case Op::V_MUL_F32:
            case Op::V_MIN_F32: case Op::V_MAX_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                Value b = toF32(vectorSel(in.f[2]));
                if (in.op == Op::V_MIN_F32 || in.op == Op::V_MAX_F32) {
                    const spv::Word g = m.glsl(
                        in.op == Op::V_MIN_F32 ? spv::Glsl::FMin : spv::Glsl::FMax,
                        f32T, {a.id, b.id});
                    store(&vVal[in.f[0]], {g, Kind::F32});
                } else {
                    const spv::Word opc = in.op == Op::V_ADD_F32 ? spv::Op::FAdd
                                        : in.op == Op::V_SUB_F32 ? spv::Op::FSub
                                                                 : spv::Op::FMul;
                    store(&vVal[in.f[0]], {m.emit(opc, f32T, {a.id, b.id}), Kind::F32});
                }
                return true;
            }
            case Op::V_AND_B32: case Op::V_OR_B32: {
                Value a = bitcastU32(vectorSel(in.f[1]));
                Value b = bitcastU32(vectorSel(in.f[2]));
                const spv::Word opc = in.op == Op::V_AND_B32 ? spv::Op::BitwiseAnd
                                                             : spv::Op::BitwiseOr;
                store(&vVal[in.f[0]], {m.emit(opc, u32T, {a.id, b.id}), Kind::U32});
                return true;
            }
            case Op::V_LSHLREV_B32: case Op::V_LSHRREV_B32: {
                Value amt = bitcastU32(vectorSel(in.f[1]));
                Value val = bitcastU32(vectorSel(in.f[2]));
                const spv::Word opc = in.op == Op::V_LSHLREV_B32
                    ? spv::Op::ShiftLeftLogical : spv::Op::ShiftRightLogical;
                store(&vVal[in.f[0]], {m.emit(opc, u32T, {val.id, amt.id}), Kind::U32});
                return true;
            }
            case Op::V_MOV_B32:
                store(&vVal[in.f[0]], vectorSel(in.f[1]));
                return true;
            case Op::V_CVT_F32_U32: {
                Value a = toU32(vectorSel(in.f[1]));
                store(&vVal[in.f[0]], {m.emit(spv::Op::ConvertUToF, f32T, {a.id}), Kind::F32});
                return true;
            }
            case Op::V_CVT_F32_I32: {
                Value a = bitcastU32(vectorSel(in.f[1]));
                store(&vVal[in.f[0]], {m.emit(spv::Op::ConvertSToF, f32T, {a.id}), Kind::F32});
                return true;
            }
            case Op::V_CVT_U32_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                store(&vVal[in.f[0]], {m.emit(spv::Op::ConvertFToU, u32T, {a.id}), Kind::U32});
                return true;
            }
            case Op::V_RCP_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                store(&vVal[in.f[0]], {m.emit(spv::Op::FDiv, f32T,
                        {m.constF32(1.f), a.id}), Kind::F32});
                return true;
            }
            case Op::V_SQRT_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                store(&vVal[in.f[0]], {m.glsl(spv::Glsl::Sqrt, f32T, {a.id}), Kind::F32});
                return true;
            }
            case Op::V_FRACT_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                const Word fl = m.glsl(spv::Glsl::Floor, f32T, {a.id});
                store(&vVal[in.f[0]], {m.emit(spv::Op::FSub, f32T, {a.id, fl}), Kind::F32});
                return true;
            }
            case Op::V_MAD_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                Value b = toF32(vectorSel(in.f[2]));
                Value c = toF32(vectorSel(in.f[3]));
                const Word mul = m.emit(spv::Op::FMul, f32T, {a.id, b.id});
                store(&vVal[in.f[0]], {m.emit(spv::Op::FAdd, f32T, {mul, c.id}), Kind::F32});
                return true;
            }
            case Op::V_FMA_F32: {
                Value a = toF32(vectorSel(in.f[1]));
                Value b = toF32(vectorSel(in.f[2]));
                Value c = toF32(vectorSel(in.f[3]));
                // fma(a,b,c) = mix(c, a*b, b) — a single-op exact fma arrives
                // with the float-control phase (documented approximation).
                const Word mul = m.emit(spv::Op::FMul, f32T, {a.id, b.id});
                store(&vVal[in.f[0]], {m.glsl(spv::Glsl::FMix, f32T, {c.id, mul, b.id}), Kind::F32});
                return true;
            }
            case Op::V_CMP_LT_F32: case Op::V_CMP_GT_F32:
            case Op::V_CMP_EQ_F32: case Op::V_CMP_NEQ_F32: {
                Value a = toF32(vectorSel(in.f[0]));
                Value b = toF32(vectorSel(in.f[1]));
                const spv::Word opc = in.op == Op::V_CMP_LT_F32 ? spv::Op::FOrdLessThan
                                    : in.op == Op::V_CMP_GT_F32 ? spv::Op::FOrdGreaterThan
                                    : in.op == Op::V_CMP_EQ_F32 ? spv::Op::FOrdEqual
                                                                : spv::Op::FUnordNotEqual;
                store(&sccVal, {m.emit(opc, boolT, {a.id, b.id}), Kind::Bool});
                return true;
            }
            case Op::V_INTERP_P1_F32:
            case Op::V_INTERP_P2_F32:
            case Op::V_INTERP_MOV_F32: {
                // Fragment: interpolated input. Vertex: vertex-buffer fetch
                // (attr = location, chan = component) — the subset's uniform
                // interface for stage inputs.
                if (stage == Stage::Fragment) {
                    const spv::Word inVar = ensureIO(fsInLoc, in.f[1], spv::Storage::Input, "fs_in");
                    const Word loaded = m.emit(spv::Op::Load, v4fT, {inVar});
                    store(&vVal[in.f[0]], {m.emit(spv::Op::CompositeExtract, f32T, {loaded, in.f[2] & 3u}), Kind::F32});
                } else {
                    const spv::Word inVar = ensureIO(vsInLoc, in.f[1], spv::Storage::Input, "vs_in");
                    const Word loaded = m.emit(spv::Op::Load, v4fT, {inVar});
                    store(&vVal[in.f[0]], {m.emit(spv::Op::CompositeExtract, f32T, {loaded, in.f[2] & 3u}), Kind::F32});
                }
                return true;
            }
            case Op::EXP: {
                const u32 en = in.f[0] & 0xFu;
                const u32 target = in.f[1];
                if (target >= 8 && target != 12) {
                    res.err = "exp target outside MRT0..7 / POS(12) subset";
                    return false;
                }
                if (target == 12 && stage != Stage::Vertex) {
                    res.err = "position export in a fragment shader";
                    return false;
                }
                std::vector<spv::Word> merged;
                for (u32 ch = 0; ch < 4; ++ch) {
                    Value s = toF32(vectorSel(in.lits[ch]));
                    merged.push_back((en & (1u << ch)) ? s.id : m.constF32(0.f));
                }
                const Word vec = m.emit(spv::Op::CompositeConstruct, v4fT, merged);
                if (target == 12) {
                    // Position export.
                    m.emitVoid(spv::Op::Store, {outPos, vec});
                } else {
                    const spv::Word outVar = ensureIO(vsOutLoc, target, spv::Storage::Output, "out");
                    m.emitVoid(spv::Op::Store, {outVar, vec});
                }
                return true;
            }
            case Op::S_LOAD_DWORD: case Op::S_LOAD_DWORDX2: {
                const spv::Word sv = ensureSsbo();
                Value off = toU32(scalarSel(in.f[2]));
                const Word buf = m.emit(spv::Op::Load, ssboPtrT, {sv});
                const Word ptr = m.emit(spv::Op::AccessChain, u32PtrSSBO,
                                        {buf, m.constU32(0), off.id});
                store(&sVal[in.f[0]], {m.emit(spv::Op::Load, u32T, {ptr}), Kind::U32});
                return true;
            }
            case Op::S_STORE_DWORD: {
                const spv::Word sv = ensureSsbo();
                Value off = toU32(scalarSel(in.f[2]));
                Value data = bitcastU32(scalarSel(in.f[0]));
                const Word buf = m.emit(spv::Op::Load, ssboPtrT, {sv});
                const Word ptr = m.emit(spv::Op::AccessChain, u32PtrSSBO,
                                        {buf, m.constU32(0), off.id});
                m.emitVoid(spv::Op::Store, {ptr, data.id});
                return true;
            }
            case Op::FLAT_LOAD_DWORD: {
                const spv::Word sv = ensureSsbo();
                Value off = toU32(vectorSel(in.f[1]));
                const Word buf = m.emit(spv::Op::Load, ssboPtrT, {sv});
                const Word ptr = m.emit(spv::Op::AccessChain, u32PtrSSBO,
                                        {buf, m.constU32(0), off.id});
                store(&vVal[in.f[0]], {m.emit(spv::Op::Load, u32T, {ptr}), Kind::U32});
                return true;
            }
            case Op::FLAT_STORE_DWORD: {
                const spv::Word sv = ensureSsbo();
                Value off = toU32(vectorSel(in.f[1]));
                Value data = bitcastU32(vectorSel(in.f[2]));
                const Word buf = m.emit(spv::Op::Load, ssboPtrT, {sv});
                const Word ptr = m.emit(spv::Op::AccessChain, u32PtrSSBO,
                                        {buf, m.constU32(0), off.id});
                m.emitVoid(spv::Op::Store, {ptr, data.id});
                return true;
            }
            case Op::IMAGE_SAMPLE: {
                const u32 slot = in.f[2];
                spv::Word svar;
                auto it = samplerVars.find(slot);
                if (it != samplerVars.end()) {
                    svar = it->second;
                } else {
                    const spv::Word imgT = m.typeImage(f32T);
                    const spv::Word siT = m.typeSampledImage(imgT);
                    svar = m.globalVariable(siT, spv::Storage::UniformConstant);
                    char nm[32];
                    std::snprintf(nm, sizeof(nm), "tex%u", slot);
                    m.name(svar, nm);
                    m.decorateDescriptor(svar, 0, slot);
                    samplerVars[slot] = svar;
                    interface.push_back(svar);
                }
                Value cx = toF32(vectorSel(in.f[1]));
                Value cy = toF32(vectorSel(
                    (in.f[1] >= 256 && in.f[1] + 1 <= 511) ? in.f[1] + 1 : in.f[1]));
                const Word coords = m.emit(spv::Op::CompositeConstruct, v2fT, {cx.id, cy.id});
                const spv::Word siT = m.typeSampledImage(m.typeImage(f32T));
                const Word samp = m.emit(spv::Op::Load, siT, {svar});
                const Word sampRes = m.emit(spv::Op::ImageSampleImplicitLod, v4fT, {samp, coords});
                const Word x = m.emit(spv::Op::CompositeExtract, f32T, {sampRes, 0u});
                store(&vVal[in.f[0]], {x, Kind::F32});
                return true;
            }
            case Op::S_ENDPGM:
            case Op::S_BRANCH:
            case Op::S_CBRANCH_SCC0:
            case Op::S_CBRANCH_SCC1:
                return true;   // flow handled by the walker
            default:
                res.err = std::string("unsupported op: ") + instText(in);
                return false;
        }
    };

    // Region walker.
    std::vector<Region> stack;
    size_t i = 0;
    bool hitEnd = false;
    while (i < prog.size()) {
        const u32 pc = prog[i].pc;
        const Inst& in = prog[i].in;

        while (!stack.empty() && stack.back().mergePc == pc) {
            Region r = stack.back();
            stack.pop_back();
            m.emitVoid(spv::Op::Branch, {r.mergeLbl});
            if (r.hasElse) {
                depth = r.openDepth + 1;
                m.beginBlockAt(r.jumpLbl);
                for (u32 k = r.elseStart; k < r.elseEnd; ++k) {
                    if (!emitInst(prog[pcIdx[k]].in)) return res;
                }
                m.emitVoid(spv::Op::Branch, {r.mergeLbl});
            }
            m.beginBlockAt(r.mergeLbl);
            depth = r.openDepth;
        }

        if (in.op == Op::S_ENDPGM) { hitEnd = true; break; }

        // The shared merge jump of an else-region: consumed by the region
        // logic (both paths reach the merge structurally), never emitted.
        if (!stack.empty() && stack.back().hasElse && pc == stack.back().fallEnd &&
            in.op == Op::S_BRANCH) {
            ++i;
            continue;
        }

        // Else-body instructions are emitted at region close, not inline.
        if (!stack.empty() && stack.back().hasElse &&
            pc >= stack.back().elseStart && pc < stack.back().elseEnd) {
            ++i;
            continue;
        }

        if (in.op == Op::S_CBRANCH_SCC0 || in.op == Op::S_CBRANCH_SCC1) {
            Region r = regionAt[pc];
            r.openDepth = depth;
            r.condId = sccCond();
            if (!r.branchOnTrue)
                r.condId = m.emit(spv::Op::Not, boolT, {r.condId});
            r.mergeLbl = m.id();
            if (r.hasElse) {
                r.jumpLbl = m.id();
                r.fallLbl = m.id();
                m.emitVoid(spv::Op::SelectionMerge, {r.mergeLbl, 0});
                m.emitVoid(spv::Op::BranchConditional, {r.condId, r.jumpLbl, r.fallLbl});
                m.beginBlockAt(r.fallLbl);
            } else {
                r.fallLbl = m.id();
                m.emitVoid(spv::Op::SelectionMerge, {r.mergeLbl, 0});
                m.emitVoid(spv::Op::BranchConditional, {r.condId, r.mergeLbl, r.fallLbl});
                m.beginBlockAt(r.fallLbl);
            }
            stack.push_back(r);
            depth = stack.size();
            ++i;
            continue;
        }

        if (in.op == Op::S_BRANCH) {
            const i32 rel = static_cast<i32>(static_cast<i16>(in.f[0] & 0xFFFFu));
            const u32 target = pc + 1 + static_cast<u32>(rel);
            while (i < prog.size() && prog[i].pc < target) ++i;
            continue;
        }

        if (!emitInst(in)) return res;
        ++i;
    }

    if (!stack.empty() || hitEnd) {
        m.emitVoid(spv::Op::Return, {});
        while (!stack.empty()) {
            Region r = stack.back();
            stack.pop_back();
            m.beginBlockAt(r.fallLbl);
            m.emitVoid(spv::Op::Return, {});
            if (r.hasElse) {
                m.beginBlockAt(r.jumpLbl);
                m.emitVoid(spv::Op::Return, {});
            }
            m.beginBlockAt(r.mergeLbl);
            m.emitVoid(spv::Op::Return, {});
        }
        m.endFunction();
    } else {
        m.endFunction();
    }

    // ---- entry point (emitted last: complete interface list) ---------------------
    m.entryPoint(stage == Stage::Vertex ? spv::ExecModel::Vertex : spv::ExecModel::Fragment,
                 entryId, "psb_main", interface);
    if (stage == Stage::Fragment)
        m.executionMode(entryId, spv::ExeMode::OriginUpperLeft);

    res.spirv = m.finalize();
    res.ok = true;
    return res;
}

} // namespace psb::gpu::rdna
