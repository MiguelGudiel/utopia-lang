# LLVM Code Generation

Utopia compiles to LLVM IR and lets LLVM handle optimization and native lowering.

## Pipeline

```
Lexer → Preprocessor → Parser → Sema (DeclCollector, TypeChecker,
EffectAnalyzer, ControlFlowAnalyzer) → CodeGen → LLVM opt → .o/.ll/.s → Linker
```

1. **CodeGen** produces LLVM IR per translation unit, with debug metadata (when `-g` is passed).
2. **Optimization**: the `PassBuilder` runs the O0 default pipeline or the per-module pipeline for the requested `-O` level.
3. **Emission**: object files always; `.ll` and `.s` optionally (`--emit-llvm`, `--emit-asm`).
4. **Linking**: `clang` (or the Android NDK toolchain) for executables/shared libraries, `llvm-ar` for static archives.

## Codegen details

### Mangling

Names are mangled with an Itanium-ABI-style scheme (`_Z...`):

- Constructors: `_ZN<len><class><len>ctorC1E<params>` (e.g. `_ZN5InnerC1Ei`).
- Destructors: `...D1Ev`.
- Template instantiations mangle their arguments into the name (`Box_int`, `Memory.unique_ptr_Widget`).
- `@export` and `@extern` symbols keep their unmangled names.

### Calling conventions

- Methods receive `this` as their first parameter (a pointer).
- R-value references are passed as pointers and marked appropriately.
- Variadic functions map to C varargs; `float` arguments are promoted to `double`.

### Attributes

LLVM attributes are emitted per function:

- `nounwind` always.
- Memory effects: inferred by the effect analyzer (`readnone`/`readonly`/`nofree`/`nosync`/`willreturn`/`mustprogress`) or from annotations.
- Pointer parameters get `nonnull`/`noalias`-style hints, `dereferenceable` and `align` based on the pointee type.
- `@inline`/`@forceInline` map to `inlinehint`/`alwaysinline`.
- `@weak` uses weak linkage.

### Vtables

Polymorphic classes emit a per-class vtable global (`_ZTV<Name>`), an array of function pointers. Constructors write the vtable address into the object's first field (`vptr`); virtual calls load `vptr[index]`.

### Global initialization

Module-level initializers are collected into `llvm.global_ctors` and executed by the runtime/JIT startup.

## Lifetime and optimization support

- `llvm.lifetime.start`/`llvm.lifetime.end` intrinsics bound stack allocation lifetimes.
- RVO and temporary materialization keep destructor-based values correct under optimization.
- Numeric promotion (e.g. mixed-width arithmetic) is lowered to LLVM `sext`/`zext`/`fpext` chains.
