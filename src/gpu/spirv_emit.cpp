// ============================================================================
//  PSBET — spirv_emit.cpp
//  Minimal SPIR-V 1.0 module builder. See header for scope + provenance.
// ============================================================================
#include "spirv_emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psb::gpu::spv {

namespace {
constexpr Word kMagic        = 0x07230203;
constexpr Word kVersion      = 0x00010000;  // SPIR-V 1.0
constexpr Word kGeneratorRev = 0x000B0001;  // tool id 11, revision 1
constexpr Word kSchema       = 0;

const char* kGlslExtName = "GLSL.std.450";

Word floatBits(float f) { Word w; std::memcpy(&w, &f, 4); return w; }
} // namespace

Module::Module() {
    hdr_.push_back(kMagic);
    hdr_.push_back(kVersion);
    hdr_.push_back(kGeneratorRev);
    hdr_.push_back(0);            // bound — patched in finalize()
    hdr_.push_back(kSchema);
}

Word Module::id() { return nextId_++; }

// Packed encoding: word0 = (totalWords << 16) | opcode, then operands.
void Module::pushInst(std::vector<Word>& out, Word opcode,
                      const std::vector<Word>& operands) {
    out.push_back((static_cast<Word>(operands.size() + 1) << 16) | opcode);
    for (Word o : operands) out.push_back(o);
}

void Module::appendStr(std::vector<Word>& out, const char* s) {
    const size_t len = std::strlen(s);
    const size_t litWords = len / 4 + 1;   // NUL-terminated, word-padded
    for (size_t i = 0; i < litWords; ++i) {
        Word w = 0;
        const size_t rem = len - i * 4;
        std::memcpy(&w, s + i * 4, std::min<size_t>(4, rem));
        out.push_back(w);
    }
}

void Module::capability(Word cap) {
    pushInst(secCaps_, Op::Capability, {cap});
}

void Module::extInstImport(Word resultId, const char* name) {
    std::vector<Word> ops{resultId};
    appendStr(ops, name);
    pushInst(secImports_, Op::ExtInstImport, ops);
    glslSet_ = resultId;
}

void Module::memoryModel(Word addressing, Word model) {
    pushInst(secModel_, Op::MemoryModel, {addressing, model});
}

void Module::entryPoint(ExecModel model, Word entryId, const char* name,
                        const std::vector<Word>& interface) {
    std::vector<Word> ops{static_cast<Word>(model), entryId};
    appendStr(ops, name);
    for (Word v : interface) ops.push_back(v);
    pushInst(secEntry_, Op::EntryPoint, ops);
}

void Module::executionMode(Word entryId, Word mode, const std::vector<Word>& literals) {
    std::vector<Word> ops{entryId, mode};
    for (Word l : literals) ops.push_back(l);
    pushInst(secModes_, Op::ExecutionMode, ops);
}

void Module::name(Word target, const char* n) {
    std::vector<Word> ops{target};
    appendStr(ops, n);
    pushInst(secDebug_, Op::Name, ops);
}

void Module::decorate(Word target, Word decoration, const std::vector<Word>& literals) {
    std::vector<Word> ops{target, decoration};
    for (Word l : literals) ops.push_back(l);
    pushInst(secAnnot_, Op::Decorate, ops);
}

void Module::memberDecorate(Word structType, Word member, Word decoration,
                            const std::vector<Word>& literals) {
    std::vector<Word> ops{structType, member, decoration};
    for (Word l : literals) ops.push_back(l);
    pushInst(secAnnot_, Op::MemberDecorate, ops);
}

void Module::decorateLocation(Word target, u32 location) {
    decorate(target, Dec::Location, {location});
}
void Module::decorateDescriptor(Word target, u32 set, u32 binding) {
    decorate(target, Dec::DescriptorSet, {set});
    decorate(target, Dec::Binding, {binding});
}
void Module::decorateBuiltin(Word target, Word builtin) {
    decorate(target, Dec::BuiltIn, {builtin});
}

// --- types ---------------------------------------------------------------------

Word Module::declareType(const std::vector<Word>& key) {
    // key: {opcode, placeholder(0), operands...}
    for (size_t i = 0; i < typeKeys_.size(); ++i)
        if (typeKeys_[i] == key) return typeIds_[i];
    Word res = id();
    std::vector<Word> ops{res};
    for (size_t i = 2; i < key.size(); ++i) ops.push_back(key[i]);
    pushInst(secTypes_, key[0], ops);
    typeKeys_.push_back(key);
    typeIds_.push_back(res);
    return res;
}

Word Module::typeVoid()  { return declareType({Op::TypeVoid, 0}); }
Word Module::typeBool()  { return declareType({Op::TypeBool, 0}); }
Word Module::typeF32()   { return declareType({Op::TypeFloat, 0, 32}); }
Word Module::typeU32()   { return declareType({Op::TypeInt, 0, 32, 0}); }
Word Module::typeS32()   { return declareType({Op::TypeInt, 0, 32, 1}); }
Word Module::typeVec(Word comp, u32 n) {
    return declareType({Op::TypeVector, 0, comp, n});
}
Word Module::typeVec2F32() { return typeVec(typeF32(), 2); }
Word Module::typeVec3F32() { return typeVec(typeF32(), 3); }
Word Module::typeVec4F32() { return typeVec(typeF32(), 4); }
Word Module::typeFunction(Word ret, const std::vector<Word>& params) {
    std::vector<Word> key{Op::TypeFunction, 0, ret};
    for (Word p : params) key.push_back(p);
    return declareType(key);
}

Word Module::typePointer(Storage storage, Word pointee) {
    return declareType({Op::TypePointer, 0, static_cast<Word>(storage), pointee});
}

Word Module::typeRuntimeArray(Word elem, u32 stride) {
    // OpTypeRuntimeArray <id> <elem type> — element count comes from OpConstant
    // in an OpTypeArray, but runtime arrays use Length = 0 (no operand).
    Word rt = declareType({Op::TypeRuntimeArray, 0, elem});
    decorate(rt, 6 /* ArrayStride */, {stride});
    return rt;
}

Word Module::typeStruct(const std::vector<Word>& members, bool block) {
    std::vector<Word> key{Op::TypeStruct, 0};
    for (Word m : members) key.push_back(m);
    const Word sid = declareType(key);
    if (block) decorate(sid, Dec::Block, {});
    for (size_t i = 0; i < members.size(); ++i)
        memberDecorate(sid, static_cast<Word>(i), Dec::Offset, {static_cast<Word>(i * 4)});
    return sid;
}

Word Module::typeImage(Word sampledType) {
    // 2D, Unknown depth, non-array, single-sample, sampled=1, format Unknown.
    return declareType({Op::TypeImage, 0, sampledType, 1 /*Dim2D*/, 0, 0, 0, 1, 0});
}

Word Module::typeSampler() {
    return declareType({Op::TypeSampler, 0});
}

Word Module::typeSampledImage(Word imageType) {
    return declareType({Op::TypeSampledImage, 0, imageType});
}

// --- constants --------------------------------------------------------------------

Word Module::declareConst(const std::vector<Word>& key) {
    for (size_t i = 0; i < constKeys_.size(); ++i)
        if (constKeys_[i] == key) return constIds_[i];
    Word res = id();
    std::vector<Word> ops{res};
    for (size_t i = 2; i < key.size(); ++i) ops.push_back(key[i]);
    pushInst(secTypes_, key[0], ops);   // constants live in the types section
    constKeys_.push_back(key);
    constIds_.push_back(res);
    return res;
}

Word Module::constF32(float v) {
    return declareConst({Op::Constant, 0, typeF32(), floatBits(v)});
}
Word Module::constU32(u32 v) {
    return declareConst({Op::Constant, 0, typeU32(), v});
}
Word Module::constS32(i32 v) {
    return declareConst({Op::Constant, 0, typeS32(), static_cast<Word>(v)});
}
Word Module::constBool(bool v) {
    return declareConst({v ? Op::ConstantTrue : Op::ConstantFalse, 0, typeBool()});
}
Word Module::constVec4F32(float x, float y, float z, float w) {
    return declareConst({Op::ConstantComposite, 0, typeVec4F32(),
                         constF32(x), constF32(y), constF32(z), constF32(w)});
}
Word Module::constVec2F32(float x, float y) {
    return declareConst({Op::ConstantComposite, 0, typeVec2F32(),
                         constF32(x), constF32(y)});
}

// --- globals -------------------------------------------------------------------------

Word Module::globalVariable(Word pointeeType, Storage storage, const char* dbgName) {
    // SPIR-V: OpVariable's Result Type must be a POINTER to the pointee.
    // Callers pass the pointee; the pointer type is created here so loads
    // keep using the pointee as their result type.
    const Word ptrT = typePointer(storage, pointeeType);
    const Word res = id();
    pushInst(secGlobals_, Op::Variable,
             {ptrT, res, static_cast<Word>(storage)});
    if (dbgName) name(res, dbgName);
    return res;
}

// --- functions -------------------------------------------------------------------------

void Module::beginFunction(Word resultType, const std::vector<Word>& paramTypes,
                           Word explicitId) {
    current_ = FnCtx{};
    current_.retType = resultType;
    current_.paramTypes = paramTypes;
    current_.fnId = explicitId ? explicitId : id();

    const Word fnType = typeFunction(resultType, paramTypes);
    // OpFunction: ResultType Result Control FunctionType
    pushInst(secFuncs_, Op::Function, {resultType, current_.fnId, 0, fnType});
}

std::vector<Word> Module::functionParams() {
    std::vector<Word> ids;
    for (Word t : current_.paramTypes) {
        Word pid = id();
        pushInst(secFuncs_, Op::FunctionParameter, {t, pid});
        ids.push_back(pid);
    }
    current_.paramIds = ids;
    return ids;
}

Word Module::beginBlock() {
    Word lbl = id();
    pushInst(secFuncs_, Op::Label, {lbl});
    current_.inBlock = true;
    return lbl;
}

void Module::beginBlockAt(Word lbl) {
    pushInst(secFuncs_, Op::Label, {lbl});
    current_.inBlock = true;
}

void Module::endFunction(Word returnValue) {
    if (returnValue == 0) pushInst(secFuncs_, Op::Return, {});
    else                  pushInst(secFuncs_, Op::ReturnValue, {returnValue});
    pushInst(secFuncs_, Op::FunctionEnd, {});
    current_ = FnCtx{};
}

// --- code emission -------------------------------------------------------------------------

Word Module::emit(Word opcode, Word resultType, const std::vector<Word>& operands,
                  const char* dbgName) {
    Word res = id();
    std::vector<Word> ops;
    if (resultType) ops.push_back(resultType);
    ops.push_back(res);
    for (Word o : operands) ops.push_back(o);
    pushInst(secFuncs_, opcode, ops);
    if (dbgName) name(res, dbgName);
    return res;
}

void Module::emitVoid(Word opcode, const std::vector<Word>& operands) {
    pushInst(secFuncs_, opcode, operands);
}

Word Module::emitNoType(Word opcode, const std::vector<Word>& operands) {
    Word res = id();
    std::vector<Word> ops{res};
    for (Word o : operands) ops.push_back(o);
    pushInst(secFuncs_, opcode, ops);
    return res;
}

Word Module::glsl(Word glslOp, Word resultType, const std::vector<Word>& operands) {
    std::vector<Word> ops{resultType, 0, glslSet_, glslOp};
    Word res = id();
    ops[1] = res;
    for (Word o : operands) ops.push_back(o);
    pushInst(secFuncs_, Op::ExtInst, ops);
    return res;
}

// --- finalize / disassemble --------------------------------------------------------------------

std::vector<Word> Module::finalize() {
    hdr_[3] = nextId_;  // patch bound
    std::vector<Word> out;
    out.reserve(hdr_.size() + secCaps_.size() + secExt_.size() + secImports_.size()
                + secModel_.size() + secEntry_.size() + secModes_.size()
                + secDebug_.size() + secAnnot_.size() + secTypes_.size()
                + secGlobals_.size() + secFuncs_.size());
    out.insert(out.end(), hdr_.begin(), hdr_.end());
    out.insert(out.end(), secCaps_.begin(), secCaps_.end());
    out.insert(out.end(), secExt_.begin(), secExt_.end());
    out.insert(out.end(), secImports_.begin(), secImports_.end());
    out.insert(out.end(), secModel_.begin(), secModel_.end());
    out.insert(out.end(), secEntry_.begin(), secEntry_.end());
    out.insert(out.end(), secModes_.begin(), secModes_.end());
    out.insert(out.end(), secDebug_.begin(), secDebug_.end());
    out.insert(out.end(), secAnnot_.begin(), secAnnot_.end());
    out.insert(out.end(), secTypes_.begin(), secTypes_.end());
    out.insert(out.end(), secGlobals_.begin(), secGlobals_.end());
    out.insert(out.end(), secFuncs_.begin(), secFuncs_.end());
    return out;
}

std::string Module::disassemble() const {
    std::string out;
    char buf[72];
    size_t i = hdr_.size();
    while (i < secFuncs_.size()) {
        // Reassemble the section view: walk every section buffer in order.
        break;
    }
    // Walk the concatenated logical sections (no header).
    const std::vector<Word>* sections[] = {
        &secCaps_, &secExt_, &secImports_, &secModel_, &secEntry_, &secModes_,
        &secDebug_, &secAnnot_, &secTypes_, &secGlobals_, &secFuncs_ };
    for (const auto* sec : sections) {
        size_t p = 0;
        while (p < sec->size()) {
            const Word packed = (*sec)[p];
            const Word op = packed & 0xFFFF;
            const Word wc = packed >> 16;
            std::snprintf(buf, sizeof(buf), "op=%-3u wc=%-2u:", op, wc);
            out += buf;
            for (Word j = 1; j < wc; ++j) {
                std::snprintf(buf, sizeof(buf), " %08x", (*sec)[p + j]);
                out += buf;
            }
            out += "\n";
            if (wc == 0) break; // malformed — do not loop forever
            p += wc;
        }
    }
    return out;
}

// --- canonical pipeline shaders -------------------------------------------------------------------

SpirvShaders buildPipelineShaders() {
    SpirvShaders out;

    // ---- vertex: passthrough position + color ------------------------------
    {
        Module m;
        m.capability(Cap::Shader);
        const Word glslSet = m.id();
        m.extInstImport(glslSet, kGlslExtName);
        m.memoryModel(MemModel::Logical, MemModel::GLSL450);

        const Word voidT = m.typeVoid();
        const Word v4f   = m.typeVec4F32();
        const Word f32   = m.typeF32();

        const Word inPos    = m.globalVariable(v4f, Storage::Input, "inPos");
        const Word inColor  = m.globalVariable(v4f, Storage::Input, "inColor");
        const Word outPos   = m.globalVariable(v4f, Storage::Output, "outPos");
        const Word outColor = m.globalVariable(v4f, Storage::Output, "outColor");
        m.decorateLocation(inPos, 0);
        m.decorateLocation(inColor, 1);
        m.decorateBuiltin(outPos, Bi::Position);
        m.decorateLocation(outColor, 1);

        const Word vsMain = m.id();
        m.beginFunction(voidT, {}, vsMain);
        m.functionParams();
        m.beginBlock();
        const Word p = m.emit(Op::Load, v4f, {inPos});
        const Word c = m.emit(Op::Load, v4f, {inColor});
        // PBFF v1 uses GL-style y-up NDC; Vulkan NDC y points down. The
        // software rasterizer flips y when mapping to screen, so the Vulkan
        // backend flips here to produce byte-comparable output.
        // OpCompositeExtract takes LITERAL index words (not <id>s).
        const Word px   = m.emit(Op::CompositeExtract, f32, {p, 0});
        const Word py   = m.emit(Op::CompositeExtract, f32, {p, 1});
        const Word pz   = m.emit(Op::CompositeExtract, f32, {p, 2});
        const Word pw   = m.emit(Op::CompositeExtract, f32, {p, 3});
        const Word negY = m.emit(Op::FNegate, f32, {py});
        const Word pos2 = m.emit(Op::CompositeConstruct, v4f, {px, negY, pz, pw});
        m.emitVoid(Op::Store, {outPos, pos2});
        m.emitVoid(Op::Store, {outColor, c});
        m.endFunction();

        m.entryPoint(ExecModel::Vertex, vsMain, "psb_vs",
                     {inPos, inColor, outPos, outColor});

        out.vertex = m.finalize();
    }

    // ---- fragment: passthrough color ----------------------------------------
    {
        Module m;
        m.capability(Cap::Shader);
        const Word glslSet = m.id();
        m.extInstImport(glslSet, kGlslExtName);
        m.memoryModel(MemModel::Logical, MemModel::GLSL450);

        const Word voidT = m.typeVoid();
        const Word v4f   = m.typeVec4F32();

        const Word inColor  = m.globalVariable(v4f, Storage::Input, "inColor");
        const Word outColor = m.globalVariable(v4f, Storage::Output, "outColor");
        m.decorateLocation(inColor, 1);
        m.decorateLocation(outColor, 0);

        const Word fsMain = m.id();
        m.beginFunction(voidT, {}, fsMain);
        m.functionParams();
        m.beginBlock();
        const Word c = m.emit(Op::Load, v4f, {inColor});
        m.emitVoid(Op::Store, {outColor, c});
        m.endFunction();

        m.entryPoint(ExecModel::Fragment, fsMain, "psb_fs", {inColor, outColor});
        m.executionMode(fsMain, ExeMode::OriginUpperLeft);

        out.fragment = m.finalize();
    }

    return out;
}

u64 hashWords(const std::vector<Word>& words) {
    u64 h = 1469598103934665603ull;
    for (Word w : words) {
        for (int b = 0; b < 4; ++b) {
            h ^= static_cast<u64>((w >> (8 * b)) & 0xFF);
            h *= 1099511628211ull;
        }
    }
    return h;
}

} // namespace psb::gpu::spv
