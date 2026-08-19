#pragma once
#include "utopia/CodeGen/Intrinsics.hpp"

namespace utopia {

class IntrinsicRegistry;

/* Registers the portable SIMD intrinsics (simd_*). The implementations are
 * type-driven: the LLVM IR for every op is derived from the vector types at
 * the call site, so the same ops back the 128/256/512-bit portable layer
 * and the x86 (SSE/AVX/AVX-512) and NEON named layers. */
void registerSimdIntrinsics(IntrinsicRegistry &registry);

} // namespace utopia
