#!/usr/bin/env python3
"""Generates the SIMD prelude files for Utopia.

Outputs:
  libs/prelude/lib/Simd/Core.utp   - portable layer: ops for every fixed
                                     vector type, mapped to LLVM vector IR.
  libs/prelude/lib/Simd/X86.utp    - x86 layer: _mm/_mm256/_mm512 names
                                     (SSE/AVX/AVX-512) over the same ops.
  libs/prelude/lib/Simd/Neon.utp   - AArch64 NEON layer: v*_q names.

The C++ side (SimdIntrinsics.cpp) implements each op once, driven by the
vector types at the call site, so these files only declare signatures.
"""

import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "libs", "prelude", "lib")
OUT_DIR = os.path.join(ROOT, "Simd")

# ---------------------------------------------------------------------------
# Type universe
# ---------------------------------------------------------------------------

FLOAT_TYPES = ["float32x4", "float32x8", "float32x16",
               "float64x2", "float64x4", "float64x8"]
SIGNED_TYPES = ["int8x16", "int8x32", "int8x64",
                "int16x8", "int16x16", "int16x32",
                "int32x4", "int32x8", "int32x16",
                "int64x2", "int64x4", "int64x8"]
UNSIGNED_TYPES = ["uint8x16", "uint8x32", "uint8x64",
                  "uint16x8", "uint16x16", "uint16x32",
                  "uint32x4", "uint32x8", "uint32x16",
                  "uint64x2", "uint64x4", "uint64x8"]
BOOL_TYPES = ["boolx2", "boolx4", "boolx8",
              "boolx16", "boolx32", "boolx64"]

ALL_TYPES = FLOAT_TYPES + SIGNED_TYPES + UNSIGNED_TYPES + BOOL_TYPES
ALL_NUMERIC = FLOAT_TYPES + SIGNED_TYPES + UNSIGNED_TYPES

ELEM_OF = {t: t.split("x")[0] for t in ALL_TYPES}
BITS = {"float32": 32, "float64": 64, "int8": 8, "int16": 16, "int32": 32,
        "int64": 64, "uint8": 8, "uint16": 16, "uint32": 32, "uint64": 64,
        "bool": 1}
LANES = {t: int(t.split("x")[1]) for t in ALL_TYPES}

SHORT = {
    "float32x4": "f32x4", "float32x8": "f32x8", "float32x16": "f32x16",
    "float64x2": "f64x2", "float64x4": "f64x4", "float64x8": "f64x8",
    "int8x16": "i8x16", "int8x32": "i8x32", "int8x64": "i8x64",
    "int16x8": "i16x8", "int16x16": "i16x16", "int16x32": "i16x32",
    "int32x4": "i32x4", "int32x8": "i32x8", "int32x16": "i32x16",
    "int64x2": "i64x2", "int64x4": "i64x4", "int64x8": "i64x8",
    "uint8x16": "u8x16", "uint8x32": "u8x32", "uint8x64": "u8x64",
    "uint16x8": "u16x8", "uint16x16": "u16x16", "uint16x32": "u16x32",
    "uint32x4": "u32x4", "uint32x8": "u32x8", "uint32x16": "u32x16",
    "uint64x2": "u64x2", "uint64x4": "u64x4", "uint64x8": "u64x8",
    "boolx2": "b2", "boolx4": "b4", "boolx8": "b8",
    "boolx16": "b16", "boolx32": "b32", "boolx64": "b64",
}


def elem(t):
    return ELEM_OF[t]


def is_float(t):
    return elem(t) in ("float32", "float64")


def is_signed(t):
    return elem(t).startswith("int")


def is_unsigned(t):
    return elem(t).startswith("uint")


def mask_of(t):
    return "boolx%d" % LANES[t]


def width(t):
    return BITS[elem(t)] * LANES[t]


# ---------------------------------------------------------------------------
# Portable Core.utp generators
# ---------------------------------------------------------------------------

def _binary(t, ops):
    return ['@intrinsic("simd_%s") public %s simd_%s(%s a, %s b);'
            % (op, t, op, t, t) for op in ops]


def _unary(t, ops):
    return ['@intrinsic("simd_%s") public %s simd_%s(%s a);' % (op, t, op, t)
            for op in ops]


def _cmp(t, ops):
    return ['@intrinsic("simd_%s") public %s simd_%s(%s a, %s b);'
            % (op, mask_of(t), op, t, t) for op in ops]


def _cmp_ones(t, ops):
    return ['@intrinsic("simd_%s") public %s simd_%s(%s a, %s b);'
            % (op, t, op, t, t) for op in ops]


def _special(t):
    e = elem(t)
    n = SHORT[t]
    lines = []
    lines.append('@intrinsic("simd_splat") public %s simd_splat_%s(%s x);'
                 % (t, n, e))
    args = ", ".join("%s x%d" % (e, i) for i in range(LANES[t]))
    lines.append('@intrinsic("simd_make") public %s simd_make_%s(%s);'
                 % (t, n, args))
    lines.append('@intrinsic("simd_zero") public %s simd_zero_%s();' % (t, n))
    for op, ptr in (("load", "const %s*"), ("loadu", "const %s*"),
                    ("broadcast", "const %s*")):
        lines.append('@intrinsic("simd_%s") public %s simd_%s_%s(%s p);'
                     % (op, t, op, n, ptr % e))
    for op in ("store", "storeu"):
        lines.append('@intrinsic("simd_%s") public void simd_%s_%s(%s v, %s* p);'
                     % (op, op, n, t, e))
    return lines


def _convert(t):
    lines = []
    for s in ALL_NUMERIC:
        if s == t or LANES[s] != LANES[t] or elem(s) == elem(t):
            continue
        lines.append('@intrinsic("simd_convert") public %s simd_convert_%s(%s v);'
                     % (t, SHORT[t], s))
    return lines


def _bitcast(t):
    lines = []
    for s in ALL_NUMERIC:
        if s == t or width(s) != width(t) or elem(s) == elem(t):
            continue
        lines.append('@intrinsic("simd_bitcast") public %s simd_bitcast_%s(%s v);'
                     % (t, SHORT[t], s))
    return lines


def _layout(t):
    e = elem(t)
    idx = ", ".join("usize i%d" % i for i in range(LANES[t]))
    return [
        '@intrinsic("simd_shuffle") public %s simd_shuffle(%s a, %s b, %s);'
        % (t, t, t, idx),
        '@intrinsic("simd_extract") public %s simd_extract(%s v, usize i);'
        % (e, t),
        '@intrinsic("simd_insert") public %s simd_insert(%s v, usize i, %s x);'
        % (t, t, e),
        '@intrinsic("simd_splat_lane") public %s simd_splat_lane(%s v, usize i);'
        % (t, t),
        '@intrinsic("simd_unpacklo") public %s simd_unpacklo(%s a, %s b);'
        % (t, t, t),
        '@intrinsic("simd_unpackhi") public %s simd_unpackhi(%s a, %s b);'
        % (t, t, t),
        '@intrinsic("simd_movelh") public %s simd_movelh(%s a, %s b);'
        % (t, t, t),
        '@intrinsic("simd_movehl") public %s simd_movehl(%s a, %s b);'
        % (t, t, t),
    ]


def _reduce(t):
    e = elem(t)
    lines = []
    lines.append('@intrinsic("simd_reduce_add") public %s simd_reduce_add(%s v);'
                 % (e, t))
    if is_float(t) or is_signed(t):
        lines.append('@intrinsic("simd_reduce_min") public %s simd_reduce_min(%s v);'
                     % (e, t))
        lines.append('@intrinsic("simd_reduce_max") public %s simd_reduce_max(%s v);'
                     % (e, t))
    if is_unsigned(t):
        lines.append('@intrinsic("simd_reduce_umin") public %s simd_reduce_umin(%s v);'
                     % (e, t))
        lines.append('@intrinsic("simd_reduce_umax") public %s simd_reduce_umax(%s v);'
                     % (e, t))
    if not is_float(t):
        lines.append('@intrinsic("simd_reduce_and") public %s simd_reduce_and(%s v);'
                     % (e, t))
        lines.append('@intrinsic("simd_reduce_or") public %s simd_reduce_or(%s v);'
                     % (e, t))
        lines.append('@intrinsic("simd_reduce_xor") public %s simd_reduce_xor(%s v);'
                     % (e, t))
    return lines


def gen_core_float(t):
    return (_binary(t, ["add", "sub", "mul", "div", "min", "max", "hadd",
                        "and", "or", "xor"])
            + _unary(t, ["sqrt", "neg", "not"])
            + ['@intrinsic("simd_fma") public %s simd_fma(%s a, %s b, %s c);'
               % (t, t, t, t)]
            + _cmp(t, ["cmp_eq", "cmp_ne", "cmp_lt", "cmp_le",
                       "cmp_gt", "cmp_ge"])
            + _cmp_ones(t, ["cmp_eq_ones", "cmp_ne_ones", "cmp_lt_ones",
                            "cmp_le_ones", "cmp_gt_ones", "cmp_ge_ones"])
            + ['@intrinsic("simd_select") public %s simd_select(%s m, %s a, %s b);'
               % (t, mask_of(t), t, t)]
            + ['@intrinsic("simd_blendv") public %s simd_blendv(%s m, %s a, %s b);'
               % (t, t, t, t)]
            + _layout(t) + _special(t) + _convert(t) + _bitcast(t) + _reduce(t)
            + ['@intrinsic("simd_signmask") public int32 simd_signmask(%s v);'
               % t])


def gen_core_signed(t):
    return (_binary(t, ["add", "sub", "mul", "sdiv", "srem", "min", "max",
                        "hadd", "and", "or", "xor"])
            + _unary(t, ["neg", "not"])
            + _binary(t, ["shl", "shr"])
            + _cmp(t, ["cmp_eq", "cmp_ne", "cmp_lt", "cmp_le",
                       "cmp_gt", "cmp_ge"])
            + _cmp_ones(t, ["cmp_eq_ones", "cmp_ne_ones", "cmp_lt_ones",
                            "cmp_le_ones", "cmp_gt_ones", "cmp_ge_ones"])
            + ['@intrinsic("simd_select") public %s simd_select(%s m, %s a, %s b);'
               % (t, mask_of(t), t, t)]
            + ['@intrinsic("simd_blendv") public %s simd_blendv(%s m, %s a, %s b);'
               % (t, t, t, t)]
            + _layout(t) + _special(t) + _convert(t) + _bitcast(t) + _reduce(t)
            + ['@intrinsic("simd_signmask") public %s simd_signmask(%s v);'
               % ("int64" if LANES[t] >= 64 else "int32", t)])


def gen_core_unsigned(t):
    return (_binary(t, ["add", "sub", "mul", "udiv", "urem", "umin", "umax",
                        "hadd", "and", "or", "xor"])
            + _unary(t, ["not"])
            + _binary(t, ["shl", "ushr"])
            + _cmp(t, ["cmp_eq", "cmp_ne", "cmp_ult", "cmp_ule",
                       "cmp_ugt", "cmp_uge"])
            + _cmp_ones(t, ["cmp_eq_ones", "cmp_ne_ones", "cmp_ult_ones",
                            "cmp_ule_ones", "cmp_ugt_ones", "cmp_uge_ones"])
            + ['@intrinsic("simd_select") public %s simd_select(%s m, %s a, %s b);'
               % (t, mask_of(t), t, t)]
            + _layout(t) + _special(t) + _convert(t) + _bitcast(t) + _reduce(t)
            + ['@intrinsic("simd_signmask") public %s simd_signmask(%s v);'
               % ("int64" if LANES[t] >= 64 else "int32", t)])


def gen_core_mask(t):
    return (_binary(t, ["and", "or", "xor"])
            + _unary(t, ["not"])
            + ['@intrinsic("simd_extract") public bool simd_extract(%s m, usize i);'
               % t]
            + ['@intrinsic("simd_zero") public %s simd_zero_%s();' % (t, SHORT[t])]
            + ['@intrinsic("simd_any") public bool simd_any(%s m);' % t]
            + ['@intrinsic("simd_all") public bool simd_all(%s m);' % t]
            + ['@intrinsic("simd_bitmask") public usize simd_bitmask(%s m);' % t]
            + ['@intrinsic("simd_reduce_and") public bool simd_reduce_and(%s m);' % t]
            + ['@intrinsic("simd_reduce_or") public bool simd_reduce_or(%s m);' % t]
            + ['@intrinsic("simd_reduce_xor") public bool simd_reduce_xor(%s m);' % t])


def gen_core():
    out = ["/* AUTO-GENERATED by scripts/gen_simd_prelude.py. Do not edit. */",
           "/*",
           " * Portable SIMD layer. Every operation is declared for each fixed",
           " * vector type and lowered by the compiler to LLVM vector IR, which",
           " * each backend (SSE/AVX/AVX-512, NEON, SVE, RVV, ...) turns into",
           " * native SIMD instructions.",
           " */",
           ""]

    def section(title, types, fn):
        out.append("/* ---------------- %s ---------------- */" % title)
        for t in types:
            out.extend(fn(t))
        out.append("")

    section("float32", FLOAT_TYPES[:3], gen_core_float)
    section("float64", FLOAT_TYPES[3:], gen_core_float)
    section("int8", SIGNED_TYPES[:3], gen_core_signed)
    section("int16", SIGNED_TYPES[3:6], gen_core_signed)
    section("int32", SIGNED_TYPES[6:9], gen_core_signed)
    section("int64", SIGNED_TYPES[9:], gen_core_signed)
    section("uint8", UNSIGNED_TYPES[:3], gen_core_unsigned)
    section("uint16", UNSIGNED_TYPES[3:6], gen_core_unsigned)
    section("uint32", UNSIGNED_TYPES[6:9], gen_core_unsigned)
    section("uint64", UNSIGNED_TYPES[9:], gen_core_unsigned)
    section("boolxN mask vectors", BOOL_TYPES, gen_core_mask)
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# x86 layer
# ---------------------------------------------------------------------------

F32 = {"": "float32x4", "256": "float32x8", "512": "float32x16"}
F64 = {"": "float64x2", "256": "float64x4", "512": "float64x8"}
I8 = {"": "int8x16", "256": "int8x32", "512": "int8x64"}
I16 = {"": "int16x8", "256": "int16x16", "512": "int16x32"}
I32 = {"": "int32x4", "256": "int32x8", "512": "int32x16"}
I64 = {"": "int64x2", "256": "int64x4", "512": "int64x8"}
U32 = {"": "uint32x4", "256": "uint32x8", "512": "uint32x16"}


def x86_decl(op, ret, args, name=None, intrinsic=None):
    """@intrinsic('simd_<op>') public <ret> <name>(<args>);"""
    if name is None:
        name = op
    return '@intrinsic("simd_%s") public %s %s(%s);' % (intrinsic or op, ret,
                                                        name, args)


def gen_x86():
    out = ["/* AUTO-GENERATED by scripts/gen_simd_prelude.py. Do not edit. */",
           "/*",
           " * x86 layer: C-compatible intrinsic names (SSE/AVX/AVX-512) over",
           " * the portable SIMD ops. Compile with the required target",
           " * features (e.g. --mattr=+avx2) so the backend emits the native",
           " * instructions; without them LLVM still lowers the vector IR",
           " * correctly.",
           " */",
           ""]

    def section(title, lines):
        out.append("/* ---------------- %s ---------------- */" % title)
        out.extend(lines)
        out.append("")

    lines = []
    for op in ("add", "sub", "mul", "div", "min", "max"):
        for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                       ("_mm512", F32["512"])):
            lines.append(x86_decl(op, t, "%s a, %s b" % (t, t),
                                  name="%s_%s_ps" % (pre, op)))
        for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                       ("_mm512", F64["512"])):
            lines.append(x86_decl(op, t, "%s a, %s b" % (t, t),
                                  name="%s_%s_pd" % (pre, op)))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("sqrt", t, "%s a" % t, name="%s_sqrt_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("sqrt", t, "%s a" % t, name="%s_sqrt_pd" % pre))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"])):
        lines.append(x86_decl("hadd", t, "%s a, %s b" % (t, t),
                              name="%s_hadd_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"])):
        lines.append(x86_decl("hadd", t, "%s a, %s b" % (t, t),
                              name="%s_hadd_pd" % pre))
    section("float arithmetic (ps/pd)", lines)

    lines = []
    for op in ("and", "or", "xor"):
        for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                       ("_mm512", F32["512"])):
            lines.append(x86_decl(op, t, "%s a, %s b" % (t, t),
                                  name="%s_%s_ps" % (pre, op)))
        for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                       ("_mm512", F64["512"])):
            lines.append(x86_decl(op, t, "%s a, %s b" % (t, t),
                                  name="%s_%s_pd" % (pre, op)))
        for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                       ("_mm512", I32["512"])):
            lines.append(x86_decl(op, t, "%s a, %s b" % (t, t),
                                  name="%s_%s_si128" % (pre, op)))
    section("float/int bitwise", lines)

    lines = []
    for pre, t in (("_mm", I8[""]), ("_mm256", I8["256"]),
                   ("_mm512", I8["512"])):
        lines.append(x86_decl("add", t, "%s a, %s b" % (t, t),
                              name="%s_add_epi8" % pre))
        lines.append(x86_decl("sub", t, "%s a, %s b" % (t, t),
                              name="%s_sub_epi8" % pre))
        lines.append(x86_decl("max", t, "%s a, %s b" % (t, t),
                              name="%s_max_epi8" % pre))
        lines.append(x86_decl("min", t, "%s a, %s b" % (t, t),
                              name="%s_min_epi8" % pre))
    for pre, t in (("_mm", I16[""]), ("_mm256", I16["256"]),
                   ("_mm512", I16["512"])):
        lines.append(x86_decl("add", t, "%s a, %s b" % (t, t),
                              name="%s_add_epi16" % pre))
        lines.append(x86_decl("sub", t, "%s a, %s b" % (t, t),
                              name="%s_sub_epi16" % pre))
        lines.append(x86_decl("mul", t, "%s a, %s b" % (t, t),
                              name="%s_mullo_epi16" % pre))
        lines.append(x86_decl("max", t, "%s a, %s b" % (t, t),
                              name="%s_max_epi16" % pre))
        lines.append(x86_decl("min", t, "%s a, %s b" % (t, t),
                              name="%s_min_epi16" % pre))
        lines.append(x86_decl("shr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srai_epi16" % pre, intrinsic="shr_scalar"))
        lines.append(x86_decl("shl_scalar", t, "%s a, int32 imm" % t,
                              name="%s_slli_epi16" % pre, intrinsic="shl_scalar"))
        lines.append(x86_decl("ushr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srli_epi16" % pre, intrinsic="ushr_scalar"))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("add", t, "%s a, %s b" % (t, t),
                              name="%s_add_epi32" % pre))
        lines.append(x86_decl("sub", t, "%s a, %s b" % (t, t),
                              name="%s_sub_epi32" % pre))
        lines.append(x86_decl("mul", t, "%s a, %s b" % (t, t),
                              name="%s_mullo_epi32" % pre))
        lines.append(x86_decl("sdiv", t, "%s a, %s b" % (t, t),
                              name="%s_div_epi32" % pre))
        lines.append(x86_decl("max", t, "%s a, %s b" % (t, t),
                              name="%s_max_epi32" % pre))
        lines.append(x86_decl("min", t, "%s a, %s b" % (t, t),
                              name="%s_min_epi32" % pre))
        lines.append(x86_decl("shr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srai_epi32" % pre, intrinsic="shr_scalar"))
        lines.append(x86_decl("shl_scalar", t, "%s a, int32 imm" % t,
                              name="%s_slli_epi32" % pre, intrinsic="shl_scalar"))
        lines.append(x86_decl("ushr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srli_epi32" % pre, intrinsic="ushr_scalar"))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("add", t, "%s a, %s b" % (t, t),
                              name="%s_add_epi64" % pre))
        lines.append(x86_decl("sub", t, "%s a, %s b" % (t, t),
                              name="%s_sub_epi64" % pre))
        lines.append(x86_decl("max", t, "%s a, %s b" % (t, t),
                              name="%s_max_epi64" % pre))
        lines.append(x86_decl("min", t, "%s a, %s b" % (t, t),
                              name="%s_min_epi64" % pre))
        lines.append(x86_decl("shr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srai_epi64" % pre, intrinsic="shr_scalar"))
        lines.append(x86_decl("shl_scalar", t, "%s a, int32 imm" % t,
                              name="%s_slli_epi64" % pre, intrinsic="shl_scalar"))
        lines.append(x86_decl("ushr_scalar", t, "%s a, int32 imm" % t,
                              name="%s_srli_epi64" % pre, intrinsic="ushr_scalar"))
    section("integer arithmetic", lines)

    lines = []
    for pre, t in (("_mm", U32[""]), ("_mm256", U32["256"])):
        lines.append(x86_decl("max", t, "%s a, %s b" % (t, t),
                              name="%s_max_epu32" % pre))
        lines.append(x86_decl("min", t, "%s a, %s b" % (t, t),
                              name="%s_min_epu32" % pre))
    section("unsigned integer min/max", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        for op in ("eq", "lt", "le", "gt", "ge"):
            lines.append(x86_decl("cmp_%s_ones" % op, t, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s_ps" % (pre, op)))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        for op in ("eq", "lt", "le", "gt", "ge"):
            lines.append(x86_decl("cmp_%s_ones" % op, t, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s_pd" % (pre, op)))
    for pre, t in (("_mm", I8[""]), ("_mm256", I8["256"]),
                   ("_mm512", I8["512"])):
        for op in ("eq", "lt", "gt"):
            lines.append(x86_decl("cmp_%s_ones" % op, t, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s_epi8" % (pre, op)))
    for pre, t in (("_mm", I16[""]), ("_mm256", I16["256"]),
                   ("_mm512", I16["512"])):
        for op in ("eq", "lt", "gt"):
            lines.append(x86_decl("cmp_%s_ones" % op, t, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s_epi16" % (pre, op)))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        for op in ("eq", "lt", "gt"):
            lines.append(x86_decl("cmp_%s_ones" % op, t, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s_epi32" % (pre, op)))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("cmp_eq_ones", t, "%s a, %s b" % (t, t),
                              name="%s_cmpeq_epi64" % pre))
        lines.append(x86_decl("cmp_gt_ones", t, "%s a, %s b" % (t, t),
                              name="%s_cmpgt_epi64" % pre))
    for pre, t in (("_mm", U32[""]), ("_mm256", U32["256"]),
                   ("_mm512", U32["512"])):
        lines.append(x86_decl("cmp_ult_ones", t, "%s a, %s b" % (t, t),
                              name="%s_cmplt_epu32" % pre))
        lines.append(x86_decl("cmp_ugt_ones", t, "%s a, %s b" % (t, t),
                              name="%s_cmpgt_epu32" % pre))
    section("comparisons (all-ones results)", lines)

    lines = []
    for pre, t, m, suf in (("_mm512", F32["512"], "boolx16", "_ps"),
                           ("_mm512", F64["512"], "boolx8", "_pd"),
                           ("_mm512", I32["512"], "boolx16", "_epi32"),
                           ("_mm512", I64["512"], "boolx8", "_epi64")):
        for op in ("eq", "lt", "le", "gt", "ge"):
            lines.append(x86_decl("cmp_%s" % op, m, "%s a, %s b" % (t, t),
                                  name="%s_cmp%s%s_mask" % (pre, op, suf)))
    section("AVX-512 mask comparisons", lines)

    lines = []
    for pre, t, m in (("_mm", F32[""], "boolx4"), ("_mm256", F32["256"], "boolx8"),
                      ("_mm", F64[""], "boolx2"), ("_mm256", F64["256"], "boolx4"),
                      ("_mm512", F32["512"], "boolx16"), ("_mm512", F64["512"], "boolx8"),
                      ("_mm", I32[""], "boolx4"), ("_mm256", I32["256"], "boolx8"),
                      ("_mm512", I32["512"], "boolx16"), ("_mm512", I64["512"], "boolx8")):
        suf = "_pd" if t.startswith("float64") else (
            "_ps" if t.startswith("float32") else (
                "_epi64" if t.startswith("int64") else "_epi32"))
        lines.append(x86_decl("select", t, "%s k, %s a, %s b" % (m, t, t),
                              name="%s_mask_blend%s" % (pre, suf),
                              intrinsic="select_swap"))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm", I8[""]), ("_mm256", I8["256"])):
        suf = "_pd" if t.startswith("float64") else (
            "_ps" if t.startswith("float32") else "_epi8")
        lines.append(x86_decl("blendv", t, "%s m, %s a, %s b" % (t, t, t),
                              name="%s_blendv%s" % (pre, suf)))
    section("select / blend", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"])):
        lines.append(x86_decl("movelh", t, "%s a, %s b" % (t, t),
                              name="%s_movelh_ps" % pre))
        lines.append(x86_decl("movehl", t, "%s a, %s b" % (t, t),
                              name="%s_movehl_ps" % pre))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("unpacklo", t, "%s a, %s b" % (t, t),
                              name="%s_unpacklo_ps" % pre))
        lines.append(x86_decl("unpackhi", t, "%s a, %s b" % (t, t),
                              name="%s_unpackhi_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("unpacklo", t, "%s a, %s b" % (t, t),
                              name="%s_unpacklo_pd" % pre))
        lines.append(x86_decl("unpackhi", t, "%s a, %s b" % (t, t),
                              name="%s_unpackhi_pd" % pre))
    for pre, t in (("_mm", I8[""]), ("_mm256", I8["256"]),
                   ("_mm512", I8["512"])):
        lines.append(x86_decl("unpacklo", t, "%s a, %s b" % (t, t),
                              name="%s_unpacklo_epi8" % pre))
        lines.append(x86_decl("unpackhi", t, "%s a, %s b" % (t, t),
                              name="%s_unpackhi_epi8" % pre))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("unpacklo", t, "%s a, %s b" % (t, t),
                              name="%s_unpacklo_epi32" % pre))
        lines.append(x86_decl("unpackhi", t, "%s a, %s b" % (t, t),
                              name="%s_unpackhi_epi32" % pre))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("unpacklo", t, "%s a, %s b" % (t, t),
                              name="%s_unpacklo_epi64" % pre))
        lines.append(x86_decl("unpackhi", t, "%s a, %s b" % (t, t),
                              name="%s_unpackhi_epi64" % pre))
    section("shuffle (movelh/movehl/unpack)", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("zero", t, "", name="%s_setzero_ps" % pre))
        lines.append(x86_decl("splat", t, "float32 x", name="%s_set1_ps" % pre))
        args = ", ".join("float32 x%d" % i for i in range(LANES[t]))
        lines.append(x86_decl("make", t, args, name="%s_set_ps" % pre,
                              intrinsic="make_rev"))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("zero", t, "", name="%s_setzero_pd" % pre))
        lines.append(x86_decl("splat", t, "float64 x", name="%s_set1_pd" % pre))
        args = ", ".join("float64 x%d" % i for i in range(LANES[t]))
        lines.append(x86_decl("make", t, args, name="%s_set_pd" % pre,
                              intrinsic="make_rev"))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        zname = "_mm_setzero_si128" if pre == "_mm" else "%s_setzero_epi32" % pre
        lines.append(x86_decl("zero", t, "", name=zname))
        lines.append(x86_decl("splat", t, "int32 x", name="%s_set1_epi32" % pre))
        args = ", ".join("int32 x%d" % i for i in range(LANES[t]))
        lines.append(x86_decl("make", t, args, name="%s_set_epi32" % pre,
                              intrinsic="make_rev"))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("splat", t, "int64 x", name="%s_set1_epi64" % pre))
        args = ", ".join("int64 x%d" % i for i in range(LANES[t]))
        lines.append(x86_decl("make", t, args, name="%s_set_epi64" % pre,
                              intrinsic="make_rev"))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("splat_lane", t, "%s a, usize i" % t,
                              name="%s_broadcastss_ps" % pre))
    section("construction", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("load", t, "const float32* p", name="%s_load_ps" % pre))
        lines.append(x86_decl("loadu", t, "const float32* p", name="%s_loadu_ps" % pre))
        lines.append(x86_decl("store", t, "float32* p, %s a" % t, name="%s_store_ps" % pre))
        lines.append(x86_decl("storeu", t, "float32* p, %s a" % t, name="%s_storeu_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("load", t, "const float64* p", name="%s_load_pd" % pre))
        lines.append(x86_decl("loadu", t, "const float64* p", name="%s_loadu_pd" % pre))
        lines.append(x86_decl("store", t, "float64* p, %s a" % t, name="%s_store_pd" % pre))
        lines.append(x86_decl("storeu", t, "float64* p, %s a" % t, name="%s_storeu_pd" % pre))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("load", t, "const int32* p", name="%s_load_si128" % pre))
        lines.append(x86_decl("loadu", t, "const int32* p", name="%s_loadu_si128" % pre))
        lines.append(x86_decl("store", t, "int32* p, %s a" % t, name="%s_store_si128" % pre))
        lines.append(x86_decl("storeu", t, "int32* p, %s a" % t, name="%s_storeu_si128" % pre))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("broadcast", t, "const float32* p",
                              name="%s_broadcast_ps" % pre))
    section("memory", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("fma", t, "%s a, %s b, %s c" % (t, t, t),
                              name="%s_fmadd_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("fma", t, "%s a, %s b, %s c" % (t, t, t),
                              name="%s_fmadd_pd" % pre))
    section("fused multiply-add", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("convert", t, "int32x%d v" % LANES[t],
                              name="%s_cvtepi32_ps" % pre))
        lines.append(x86_decl("convert", I32["512"], "%s v" % t,
                              name="_mm512_cvtps_epi32"))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"])):
        lines.append(x86_decl("convert", t, "float32x%d v" % LANES[t],
                              name="%s_cvtps_epi32" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("convert", t, "int64x%d v" % LANES[t],
                              name="%s_cvtepi64_pd" % pre))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("convert", t, "float64x%d v" % LANES[t],
                              name="%s_cvtpd_epi64" % pre))
    section("converts", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("bitcast", "int32x%d" % LANES[t], "%s a" % t,
                              name="%s_castps_si128" % pre))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("bitcast", "float32x%d" % LANES[t], "%s a" % t,
                              name="%s_castsi128_ps" % pre))
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("bitcast", "float64x%d" % LANES[t], "%s a" % t,
                              name="%s_castps_pd" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("bitcast", "float32x%d" % LANES[t], "%s a" % t,
                              name="%s_castpd_ps" % pre))
        lines.append(x86_decl("bitcast", "int64x%d" % LANES[t], "%s a" % t,
                              name="%s_castpd_si128" % pre))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("bitcast", "float64x%d" % LANES[t], "%s a" % t,
                              name="%s_castsi128_pd" % pre))
    section("bitcasts", lines)

    lines = []
    for pre, t in (("_mm", F32[""]), ("_mm256", F32["256"]),
                   ("_mm512", F32["512"])):
        lines.append(x86_decl("extract", "float32", "%s a, usize i" % t,
                              name="%s_extract_ps" % pre))
        lines.append(x86_decl("signmask", "int32", "%s a" % t,
                              name="%s_movemask_ps" % pre))
    for pre, t in (("_mm", F64[""]), ("_mm256", F64["256"]),
                   ("_mm512", F64["512"])):
        lines.append(x86_decl("extract", "float64", "%s a, usize i" % t,
                              name="%s_extract_pd" % pre))
        lines.append(x86_decl("signmask", "int32", "%s a" % t,
                              name="%s_movemask_pd" % pre))
    for pre, t in (("_mm", I8[""]), ("_mm256", I8["256"])):
        lines.append(x86_decl("signmask", "int32", "%s a" % t,
                              name="%s_movemask_epi8" % pre))
    lines.append(x86_decl("signmask", "int64", I8["512"] + " a",
                          name="_mm512_movemask_epi8"))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("extract", "int32", "%s a, usize i" % t,
                              name="%s_extract_epi32" % pre))
    for pre, t in (("_mm", I64[""]), ("_mm256", I64["256"]),
                   ("_mm512", I64["512"])):
        lines.append(x86_decl("extract", "int64", "%s a, usize i" % t,
                              name="%s_extract_epi64" % pre))
    section("extract / movemask", lines)

    lines = []
    for pre, t in (("_mm", I8[""]), ("_mm256", I8["256"]),
                   ("_mm512", I8["512"])):
        lines.append(x86_decl("abs", t, "%s a" % t, name="%s_abs_epi8" % pre))
    for pre, t in (("_mm", I16[""]), ("_mm256", I16["256"]),
                   ("_mm512", I16["512"])):
        lines.append(x86_decl("abs", t, "%s a" % t, name="%s_abs_epi16" % pre))
    for pre, t in (("_mm", I32[""]), ("_mm256", I32["256"]),
                   ("_mm512", I32["512"])):
        lines.append(x86_decl("abs", t, "%s a" % t, name="%s_abs_epi32" % pre))
    section("absolute value", lines)

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# NEON layer
# ---------------------------------------------------------------------------

def gen_neon():
    out = ["/* AUTO-GENERATED by scripts/gen_simd_prelude.py. Do not edit. */",
           "/*",
           " * AArch64 NEON layer: v*_q names (128-bit) over the portable SIMD",
           " * ops. On aarch64 these lower to the native vadd/vmul/vmin...",
           " * instructions; on other targets they still work via portable",
           " * vector IR.",
           " */",
           ""]

    def section(title, lines):
        out.append("/* ---------------- %s ---------------- */" % title)
        out.extend(lines)
        out.append("")

    def d(op, ret, args, name):
        return '@intrinsic("simd_%s") public %s %s(%s);' % (op, ret, name,
                                                            args)

    lines = []
    for e, t in (("f32", "float32x4"), ("f64", "float64x2")):
        for op in ("add", "sub", "mul", "min", "max"):
            lines.append(d(op, t, "%s a, %s b" % (t, t), "v%sq_%s" % (op, e)))
        lines.append(d("div", t, "%s a, %s b" % (t, t), "vdivq_%s" % e))
        for op in ("abs", "neg", "sqrt"):
            lines.append(d(op, t, "%s a" % t, "v%sq_%s" % (op, e)))
        lines.append(d("fma", t, "%s a, %s b, %s c" % (t, t, t),
                       "vfmaq_%s" % e))
        lines.append(d("and", t, "%s a, %s b" % (t, t), "vandq_%s" % e))
        lines.append(d("or", t, "%s a, %s b" % (t, t), "vorrq_%s" % e))
        lines.append(d("xor", t, "%s a, %s b" % (t, t), "veorq_%s" % e))
        lines.append(d("not", t, "%s a" % t, "vmvnq_%s" % e))
    for op, m, t, mname, s in (("eq", "ceq", "float32x4", "boolx4", "f32"),
                               ("lt", "clt", "float32x4", "boolx4", "f32"),
                               ("le", "cle", "float32x4", "boolx4", "f32"),
                               ("gt", "cgt", "float32x4", "boolx4", "f32"),
                               ("ge", "cge", "float32x4", "boolx4", "f32"),
                               ("eq", "ceq", "float64x2", "boolx2", "f64"),
                               ("lt", "clt", "float64x2", "boolx2", "f64"),
                               ("le", "cle", "float64x2", "boolx2", "f64"),
                               ("gt", "cgt", "float64x2", "boolx2", "f64"),
                               ("ge", "cge", "float64x2", "boolx2", "f64")):
        lines.append(d("cmp_%s" % op, mname, "%s a, %s b" % (t, t),
                       "v%s%s" % (m, "q_%s" % s)))
    section("float", lines)

    ints = [("s8", "int8x16"), ("s16", "int16x8"), ("s32", "int32x4"),
            ("s64", "int64x2"), ("u8", "uint8x16"), ("u16", "uint16x8"),
            ("u32", "uint32x4"), ("u64", "uint64x2")]
    mask_of_elem = {"s8": "boolx16", "s16": "boolx8", "s32": "boolx4",
                    "u8": "boolx16", "u16": "boolx8", "u32": "boolx4"}
    lines = []
    for e, t in ints:
        for op in ("add", "sub"):
            lines.append(d(op, t, "%s a, %s b" % (t, t), "v%sq_%s" % (op, e)))
        for op in ("min", "max"):
            lines.append(d(op, t, "%s a, %s b" % (t, t), "v%sq_%s" % (op, e)))
        if e not in ("s64", "u64"):
            lines.append(d("mul", t, "%s a, %s b" % (t, t), "vmulq_%s" % e))
            lines.append(d("cmp_eq", mask_of_elem[e], "%s a, %s b" % (t, t),
                           "vceqq_%s" % e))
            lines.append(d("cmp_%s" % ("gt" if e.startswith("s") else "ugt"),
                           mask_of_elem[e], "%s a, %s b" % (t, t),
                           "vcgtq_%s" % e))
        lines.append(d("not", t, "%s a" % t, "vmvnq_%s" % e))
        lines.append(d("and", t, "%s a, %s b" % (t, t), "vandq_%s" % e))
        lines.append(d("or", t, "%s a, %s b" % (t, t), "vorrq_%s" % e))
        lines.append(d("xor", t, "%s a, %s b" % (t, t), "veorq_%s" % e))
        lines.append(d("abs", t, "%s a" % t, "vabsq_%s" % e))
        lines.append(d("neg", t, "%s a" % t, "vnegq_%s" % e))
        lines.append(d("shl", t, "%s a, %s b" % (t, t), "vshlq_%s" % e))
        lines.append(d("shr", t, "%s a, %s b" % (t, t), "vshrq_%s" % e))
    section("int/uint", lines)

    lines = []
    for e, t, et in (("f32", "float32x4", "float32"), ("f64", "float64x2", "float64"),
                     ("s8", "int8x16", "int8"), ("s16", "int16x8", "int16"),
                     ("s32", "int32x4", "int32"), ("s64", "int64x2", "int64"),
                     ("u8", "uint8x16", "uint8"), ("u16", "uint16x8", "uint16"),
                     ("u32", "uint32x4", "uint32"), ("u64", "uint64x2", "uint64")):
        lines.append(d("splat", t, "%s x" % et, "vdupq_n_%s" % e))
        lines.append(d("zero", t, "", "vdupq_n_%s" % e))
        lines.append(d("load", t, "const %s* p" % et, "vld1q_%s" % e))
        lines.append(d("store", t, "%s v, %s* p" % (t, et), "vst1q_%s" % e))
        lines.append(d("extract", et, "%s a, usize i" % t, "vgetq_lane_%s" % e))
        lines.append(d("insert", t, "%s a, usize i, %s x" % (t, et),
                       "vsetq_lane_%s" % e))
    section("construction / memory", lines)

    lines = []
    for op, ret, arg, name in (
            ("convert", "float32x4", "int32x4", "vcvtq_f32_s32"),
            ("convert", "float32x4", "uint32x4", "vcvtq_f32_u32"),
            ("convert", "int32x4", "float32x4", "vcvtq_s32_f32"),
            ("convert", "uint32x4", "float32x4", "vcvtq_u32_f32"),
            ("convert", "float64x2", "int64x2", "vcvtq_f64_s64"),
            ("convert", "float64x2", "uint64x2", "vcvtq_f64_u64"),
            ("convert", "int64x2", "float64x2", "vcvtq_s64_f64"),
            ("convert", "uint64x2", "float64x2", "vcvtq_u64_f64"),
            ("bitcast", "int32x4", "float32x4", "vreinterpretq_s32_f32"),
            ("bitcast", "float32x4", "int32x4", "vreinterpretq_f32_s32"),
            ("bitcast", "uint32x4", "float32x4", "vreinterpretq_u32_f32"),
            ("bitcast", "float32x4", "uint32x4", "vreinterpretq_f32_u32"),
            ("bitcast", "int64x2", "float64x2", "vreinterpretq_s64_f64"),
            ("bitcast", "float64x2", "int64x2", "vreinterpretq_f64_s64"),
            ("bitcast", "int64x2", "int32x4", "vreinterpretq_s64_s32"),
    ):
        lines.append(d(op, ret, "%s a" % arg, name))
    section("converts / bitcasts", lines)

    lines = []
    for e, t, et in (("f32", "float32x4", "float32"), ("f64", "float64x2", "float64"),
                     ("s8", "int8x16", "int8"), ("s16", "int16x8", "int16"),
                     ("s32", "int32x4", "int32"), ("s64", "int64x2", "int64"),
                     ("u8", "uint8x16", "uint8"), ("u16", "uint16x8", "uint16"),
                     ("u32", "uint32x4", "uint32"), ("u64", "uint64x2", "uint64")):
        lines.append(d("reduce_add", et, "%s a" % t, "vaddvq_%s" % e))
        lines.append(d("reduce_min", et, "%s a" % t, "vminvq_%s" % e))
        lines.append(d("reduce_max", et, "%s a" % t, "vmaxvq_%s" % e))
    section("reductions", lines)

    return "\n".join(out) + "\n"


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, fn in (("Core.utp", gen_core),
                     ("X86.utp", gen_x86),
                     ("Neon.utp", gen_neon)):
        path = os.path.join(OUT_DIR, name)
        with open(path, "w") as f:
            f.write(fn())
        print("wrote", path)


if __name__ == "__main__":
    main()
