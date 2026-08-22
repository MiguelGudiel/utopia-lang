# TBAA Metadata

Utopia emits LLVM **type-based alias analysis** (TBAA) metadata so the optimizer can disambiguate memory accesses aggressively while staying correct.

## Design

- A single TBAA tree rooted at `"Utopia TBAA"`.
- An `"omnipotent char"` node is used as the fallback for accesses the compiler cannot classify.
- Pointer/reference accesses use an `"any pointer"` node.
- Every builtin type has its own TBAA node.
- Records get **struct-type TBAA nodes with real field offsets**, so distinct fields of the same record never alias each other.

## Tags

| Access pattern | Metadata |
| --- | --- |
| Whole-value load/store | `getTBAAAccessTag(type)` |
| Field load/store | `getTBAAStructAccessTag(base, accessType, fieldOffset)` |
| Subscript access | `getTBAATagForExpr(...)` with computed offsets |

Unions are treated as scalar nodes (their overlapping fields cannot be disambiguated).

## What it enables

- Load/store reordering across unrelated fields.
- Vectorization and LICM that would otherwise be blocked by conservative alias assumptions.
- Correctness is preserved because struct offsets are encoded precisely: two accesses alias only if their (base, offset) pair actually overlaps.

## References

- `src/utopia/CodeGen/TBAAManager.cpp`: tree construction and tag generation.
- LLVM's `TypeBasedAliasAnalysis` pass consumes the metadata emitted by codegen.
