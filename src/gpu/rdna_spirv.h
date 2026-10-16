// ============================================================================
//  PSBET — rdna_spirv.h
//  RDNA subset v1 → SPIR-V recompiler (vertex/fragment first).
//
//  Model (documented subset semantics):
//   - SGPRs: scalar SSA values (i32 or f32 by producing op; bitcast on need)
//   - VGPRs: per-invocation SSA values (same typing)
//   - SCC:   bool (written by SOPC compares and VOPC in scalar-model mode)
//   - s_branch/s_cbranch_*: forward structured branches only (if/else).
//     Backward branches are rejected with an error (loops are a later-phase
//     capability; a recompiler that silently mis-lowers loops would be worse
//     than one that refuses).
//   - VINTRP: fragment input loads (Location=attr, Component=chan)
//   - EXP: output store (Location=target); EN4 gates channels
//   - image_sample: combined image sampler at set 0, binding = resource slot
//   - flat/smem memory: one SSBO at set 0 binding 0 (u32 element array)
//
//  Entry point name: "psb_main". Interface decorations are emitted for every
//  referenced global, so the module validates under strict drivers.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "gpu/spirv_emit.h"
#include "gpu/rdna.h"

namespace psb::gpu::rdna {

enum class Stage { Vertex, Fragment };

struct RecompileResult {
    bool ok = false;
    std::string err;
    std::vector<spv::Word> spirv;
};

// Recompiles a code word sequence. Deterministic: same input → same module.
RecompileResult recompile(const u32* code, u32 codeWords, Stage stage);

// Disassembly helper (text form; used by tests and diagnostics).
std::string disassembleRange(const u32* code, u32 codeWords);

} // namespace psb::gpu::rdna
