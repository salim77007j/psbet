// ============================================================================
//  PSBET — spirv_emit.h
//  Minimal SPIR-V 1.0 module builder (zero dependencies).
//
//  Why this exists: PSBET must translate guest GPU work into host-GPU work
//  without shipping precompiled shader blobs or requiring an offline SDK.
//  Every shader PSBET feeds to Vulkan — the backend's fixed-equivalent
//  helpers and RDNA-recompiled guest shaders — is assembled through this
//  builder, keeping the whole translation path inspectable.
//
//  Scope (deliberate): Shader capability, GLSL.std.450 extended import,
//  logical addressing, the opcodes needed by vertex/fragment translation.
//
//  Enumerant provenance: every opcode/decoration/built-in below was
//  extracted programmatically from the Khronos spirv.core.grammar.json
//  (SPIRV-Headers main) — no hand-transcribed spec values.
//
//  Layout guarantee: call order of the public API does not matter. The
//  builder buffers each logical-layout section (SPIR-V spec §2.4) and
//  concatenates them in the mandated order at finalize().
//
//  Encoding: instruction word 0 = (WordCount << 16) | Opcode, WordCount
//  counting ALL words of the instruction including word 0 (spec §2.3).
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "psbet/common.h"

namespace psb::gpu::spv {

using Word = u32;

namespace Op {
    constexpr Word Nop                    = 0;
    constexpr Word Undef                  = 1;
    constexpr Word Name                   = 5;
    constexpr Word MemberName             = 6;
    constexpr Word ExtInstImport          = 11;
    constexpr Word ExtInst                = 12;
    constexpr Word MemoryModel            = 14;
    constexpr Word EntryPoint             = 15;
    constexpr Word ExecutionMode          = 16;
    constexpr Word Capability             = 17;
    constexpr Word TypeVoid               = 19;
    constexpr Word TypeBool               = 20;
    constexpr Word TypeInt                = 21;
    constexpr Word TypeFloat              = 22;
    constexpr Word TypeVector             = 23;
    constexpr Word TypeImage              = 25;
    constexpr Word TypeSampler            = 26;
    constexpr Word TypeSampledImage       = 27;
    constexpr Word TypeRuntimeArray       = 29;
    constexpr Word TypeStruct             = 30;
    constexpr Word TypePointer            = 32;
    constexpr Word TypeFunction           = 33;
    constexpr Word ConstantTrue           = 41;
    constexpr Word ConstantFalse          = 42;
    constexpr Word Constant               = 43;
    constexpr Word ConstantComposite      = 44;
    constexpr Word Function               = 54;
    constexpr Word FunctionParameter      = 55;
    constexpr Word FunctionEnd            = 56;
    constexpr Word FunctionCall           = 57;
    constexpr Word Variable               = 59;
    constexpr Word ImageTexelPointer      = 60;
    constexpr Word Load                   = 61;
    constexpr Word Store                  = 62;
    constexpr Word AccessChain            = 65;
    constexpr Word Decorate               = 71;
    constexpr Word MemberDecorate         = 72;
    constexpr Word VectorShuffle          = 79;
    constexpr Word CompositeConstruct     = 80;
    constexpr Word CompositeExtract       = 81;
    constexpr Word CompositeInsert        = 82;
    constexpr Word SampledImage           = 86;
    constexpr Word ImageSampleImplicitLod = 87;
    constexpr Word ImageSampleExplicitLod = 88;
    constexpr Word ImageFetch             = 95;
    constexpr Word Image                  = 100;
    constexpr Word Bitcast                = 124;
    constexpr Word SNegate                = 126;
    constexpr Word FNegate                = 127;
    constexpr Word IAdd                   = 128;
    constexpr Word FAdd                   = 129;
    constexpr Word FSub                   = 131;
    constexpr Word IMul                   = 132;
    constexpr Word FMul                   = 133;
    constexpr Word UDiv                   = 134;
    constexpr Word SRem                   = 138;
    constexpr Word FDiv                   = 136;
    constexpr Word FRem                   = 140;
    constexpr Word FMod                   = 141;
    constexpr Word ConvertFToU            = 109;
    constexpr Word ConvertFToS            = 110;
    constexpr Word ConvertSToF            = 111;
    constexpr Word ConvertUToF            = 112;
    constexpr Word Select                 = 169;
    constexpr Word IEqual                 = 170;
    constexpr Word INotEqual              = 171;
    constexpr Word UGreaterThan           = 172;
    constexpr Word SGreaterThan           = 173;
    constexpr Word UGreaterThanEqual      = 174;
    constexpr Word ULessThan              = 176;
    constexpr Word SLessThan              = 177;
    constexpr Word FOrdEqual              = 180;
    constexpr Word FUnordNotEqual         = 183;
    constexpr Word FOrdLessThan           = 184;
    constexpr Word FOrdGreaterThan        = 186;
    constexpr Word FOrdLessThanEqual      = 188;
    constexpr Word FOrdGreaterThanEqual   = 190;
    constexpr Word ShiftRightLogical      = 194;
    constexpr Word ShiftRightArithmetic   = 195;
    constexpr Word ShiftLeftLogical       = 196;
    constexpr Word BitwiseOr              = 197;
    constexpr Word BitwiseXor             = 198;
    constexpr Word BitwiseAnd             = 199;
    constexpr Word Not                    = 200;
    constexpr Word DPdx                   = 207;
    constexpr Word DPdy                   = 208;
    constexpr Word Fwidth                 = 209;
    constexpr Word LoopMerge              = 246;
    constexpr Word SelectionMerge         = 247;
    constexpr Word Label                  = 248;
    constexpr Word Branch                 = 249;
    constexpr Word BranchConditional      = 250;
    constexpr Word Kill                   = 252;
    constexpr Word Return                 = 253;
    constexpr Word ReturnValue            = 254;
} // namespace Op

// GLSL.std.450 extended instruction numbers (subset).
namespace Glsl {
    constexpr Word Trunc       = 3;
    constexpr Word FAbs        = 5;
    constexpr Word Floor       = 8;
    constexpr Word Sin         = 13;
    constexpr Word Cos         = 14;
    constexpr Word Pow         = 26;
    constexpr Word Exp2        = 29;
    constexpr Word Log2        = 31;
    constexpr Word Sqrt        = 32;
    constexpr Word InverseSqrt = 33;
    constexpr Word FMin        = 39;
    constexpr Word FMax        = 40;
    constexpr Word FClamp      = 43;
    constexpr Word FMix        = 46;
    constexpr Word Step        = 48;
    constexpr Word SClamp      = 35;
}

enum class Storage : Word {
    UniformConstant = 0,
    Input           = 1,
    Uniform         = 2,
    Output          = 3,
    Workgroup       = 4,
    Private         = 6,
    Function        = 7,
    PushConstant    = 9,
    StorageBuffer   = 12,
};

enum class ExecModel : Word { Vertex = 0, Fragment = 4 };

namespace Dec {
    constexpr Word BuiltIn       = 11;
    constexpr Word NoPerspective = 13;
    constexpr Word Flat          = 14;
    constexpr Word Centroid      = 16;
    constexpr Word Block         = 2;
    constexpr Word Offset        = 35;
    constexpr Word Location      = 30;
    constexpr Word Component     = 31;
    constexpr Word Binding       = 33;
    constexpr Word DescriptorSet = 34;
}

namespace Bi {
    constexpr Word Position      = 0;
    constexpr Word PointSize     = 1;
    constexpr Word FragCoord     = 15;
    constexpr Word FrontFacing   = 17;
    constexpr Word FragDepth     = 22;
    constexpr Word VertexIndex   = 42;
    constexpr Word InstanceIndex = 43;
}

namespace Cap {
    constexpr Word Shader = 1;
}

namespace ExeMode {
    constexpr Word OriginUpperLeft = 7;
}

namespace MemModel {
    constexpr Word Logical  = 1;
    constexpr Word GLSL450  = 1;
}

class Module {
public:
    Module();

    Word id();  // allocate a fresh <id>

    // --- structure sections (call order independent) ------------------------
    void capability(Word cap);
    void extInstImport(Word resultId, const char* name); // first import = GLSL set
    void memoryModel(Word addressing, Word model);
    void entryPoint(ExecModel model, Word entryId, const char* name,
                    const std::vector<Word>& interface);
    void executionMode(Word entryId, Word mode, const std::vector<Word>& literals = {});
    void name(Word target, const char* n);

    // --- annotations ----------------------------------------------------------
    void decorate(Word target, Word decoration, const std::vector<Word>& literals = {});
    void memberDecorate(Word structType, Word member, Word decoration,
                        const std::vector<Word>& literals = {});
    void decorateLocation(Word target, u32 location);
    void decorateDescriptor(Word target, u32 set, u32 binding);
    void decorateBuiltin(Word target, Word builtin);

    // --- types (deduplicated) ----------------------------------------------------
    Word typeVoid();
    Word typeBool();
    Word typeF32();
    Word typeU32();
    Word typeS32();
    Word typeVec(Word componentType, u32 count);
    Word typeVec2F32();
    Word typeVec3F32();
    Word typeVec4F32();
    Word typeFunction(Word ret, const std::vector<Word>& params);
    Word typePointer(Storage storage, Word pointee);
    Word typeRuntimeArray(Word elem, u32 stride);       // ArrayStride decorated
    Word typeStruct(const std::vector<Word>& members, bool block);
    Word typeImage(Word sampledType);                    // 2D float, sampled
    Word typeSampler();
    Word typeSampledImage(Word imageType);

    // --- constants (deduplicated) ---------------------------------------------------
    Word constF32(float v);
    Word constU32(u32 v);
    Word constS32(i32 v);
    Word constBool(bool v);
    Word constVec4F32(float x, float y, float z, float w);
    Word constVec2F32(float x, float y);

    // --- globals -----------------------------------------------------------------------
    Word globalVariable(Word resultType, Storage storage, const char* dbgName = nullptr);

    // --- functions -----------------------------------------------------------------------
    void beginFunction(Word resultType, const std::vector<Word>& paramTypes);
    std::vector<Word> functionParams();   // call once, before first block
    Word beginBlock();                    // emits OpLabel; returns label id
    void beginBlockAt(Word lbl);          // emits OpLabel for a pre-allocated id
    void endFunction(Word returnValue = 0); // 0 = OpReturn

    // --- code emission ---------------------------------------------------------------------
    Word emit(Word opcode, Word resultType, const std::vector<Word>& operands,
              const char* dbgName = nullptr);
    void emitVoid(Word opcode, const std::vector<Word>& operands);
    Word emitNoType(Word opcode, const std::vector<Word>& operands);
    Word label() { return beginBlock(); }

    // GLSL.std.450 extended instruction.
    Word glsl(Word glslOp, Word resultType, const std::vector<Word>& operands);

    std::vector<Word> finalize();
    std::string disassemble() const;

private:
    static void appendStr(std::vector<Word>& out, const char* s);
    static void pushInst(std::vector<Word>& out, Word opcode,
                         const std::vector<Word>& operands);
    Word declareType(const std::vector<Word>& key);
    Word declareConst(const std::vector<Word>& key);

    std::vector<Word> hdr_;
    std::vector<Word> secCaps_, secExt_, secImports_, secModel_, secEntry_,
        secModes_, secDebug_, secAnnot_, secTypes_, secGlobals_, secFuncs_;

    Word nextId_ = 1;
    Word glslSet_ = 0;

    std::vector<std::vector<Word>> typeKeys_;
    std::vector<Word> typeIds_;
    std::vector<std::vector<Word>> constKeys_;
    std::vector<Word> constIds_;

    struct FnCtx {
        Word retType = 0;
        std::vector<Word> paramTypes;
        std::vector<Word> paramIds;
        Word fnId = 0;
        bool inBlock = false;
    };
    FnCtx current_{};
};

// Builds the canonical PSBET vertex+fragment pair used by the Vulkan backend's
// fixed-equivalent pipeline (NDC positions, per-vertex RGBA, passthrough).
struct SpirvShaders {
    std::vector<Word> vertex;
    std::vector<Word> fragment;
};
SpirvShaders buildPipelineShaders();

// FNV-1a 64 over the module words — shader cache keys.
u64 hashWords(const std::vector<Word>& words);

} // namespace psb::gpu::spv
