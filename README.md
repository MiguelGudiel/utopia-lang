# Utopia
A low-level programming language with Dart-inspired syntax, built on LLVM.

Utopia is a low-level programming language with a syntax heavily inspired by Dart and an execution model similar to C and C++. It compiles to native code through an LLVM backend.

## Introduction

Utopia combines a C/C++-style memory and execution model (manual allocation, pointers, references, ahead-of-time compilation to native code) with a more modern, Dart-influenced surface syntax (classes, named parameters, expression-bodied functions, annotations).

The compiler is written in C++23 and generates LLVM IR, which is then optimized and lowered to native object files through the standard LLVM code generation pipeline.

## Why Utopia

Utopia is a personal compiler and language-design project. It was created as a playground for experimenting with:

- Frontend design: lexing, parsing, and semantic analysis for a C-like language
- LLVM IR generation, including debug info (DWARF) and TBAA metadata
- Language features such as generics, operator overloading, and named parameters implemented from scratch
- Build tooling around a custom module system and project manifests

This is not a production-ready language or toolchain. There is no stability guarantee across versions, and some areas of the compiler are still evolving. It is best suited for people interested in compiler construction, LLVM, or language design who want to read or experiment with a real, working implementation.

## Features

Utopia combines a C/C++-style memory and execution model with a modern, Dart-inspired surface syntax. The following features are currently implemented, with no exceptions:

### Language core

- **Static typing** with explicit-width integers (`int8`–`int64`, `uint8`–`uint64`), floating point (`float32`/`float64`, aliased as `float`/`double`), `bool`, `char` (byte), `rune` (Unicode code point), `usize`, `int`/`uint` aliases, and `void`
- **`var`/`const` type inference**, `const`-qualified types, and canonical, deduplicated type system (pointers, references, r-value references, fixed-size arrays, function types, aliases, templates)
- **Records**: `struct`, `class`, and `union` with fields, multiple overloaded constructors, destructors, methods, and `static` members
- **Enums** with an explicit underlying integer type and constant initializers (`enum Color : int8 { Red, Green = 5 }`)
- **`typedef` aliases** and multi-part `namespace` declarations (including file-scoped namespaces) with `using` directives
- **Access control**: `public`, `private`, `protected`, plus an underscore (`_`) convention for implicit privacy
- **Inheritance** (`extends`), **interfaces** (`implements`), **abstract classes/methods**, `super` delegation and access, and polymorphic dispatch via an LLVM vtable
- **Virtual dispatch** through `@virtual`/`@override` annotations and `const` methods
- **Generics/templates** for classes, structs, unions, functions, and methods (e.g. `class Box<T>`, `T findMax<T>(T, T)`) with on-demand instantiation
- **Operator overloading** (`operator+`, `operator==`, `operator[]`, `operator=`, unary and compound-assignment operators, etc.) including free-function operators
- **Expression-bodied functions** (`=>`) and classic `if`/`else`, `while`, `for`, `switch`/`case`/`default`, `break`, `continue`, `return`, and ternary `?:`

### Memory model

- **Manual memory management** via `new`/`delete` and `new[]`/`delete[]` (with array length prefix)
- **RAII**: destructors run deterministically on scope exit, early returns, and loop exits; copy/move constructors and `operator=` enforce correct ownership semantics for records with custom destructors
- **Smart pointers** in the standard library: `unique_ptr<T>`, `shared_ptr<T>`, `weak_ptr<T>` with reference counting, plus `make_unique<T>`/`make_shared<T>` factories — mirroring the C++ API
- **Rust-style auto-deref**: member access with `.` resolves through overloaded `operator*` automatically, so `ptr.field` and `ptr.method()` work directly on smart pointers (no `->` syntax needed)
- **Pointers, references, and r-value references** with automatic dereference on member access, plus `null`

### Functions & parameters

- **Named parameters** (`{int port = 8080}`), **optional/default parameters**, and a **`required`** modifier for named parameters
- **Variadic functions** (`...`) with automatic float-to-double promotion
- **Function overloading** by signature, including `const` qualifiers
- **Function-pointer types** via the `Function` type
- **Type casting** with the `as` operator and user-defined conversion constructors

### Annotations

- **Built-in annotations**: `@extern` (with calling-convention control), `@export`, `@align`, `@packed`, `@nodiscard`, `@deprecated`, `@inline`, `@forceInline`, `@readnone`, `@readonly`, `@nosync`, `@nofree`, `@willreturn`, `@mustprogress`, `@nocapture`, `@nonnull`, `@dereferenceable`, `@weak`, `@intrinsic`, `@virtual`, `@override`
- **User-defined annotation classes** (`annotation class Route { ... }`)
- **Compile-time intrinsics**: `sizeof`/`typeof` with type reflection

### Modules & tooling

- **Module system** with `import`/`export`, an automatically loaded prelude, a standard library (`utopia:memory`, `utopia:ffi`, `utopia:io`, `utopia:test`), and the `utopia:builder` API for build scripts
- **Package manager (`yip`)**: `get`, `add`, `publish`, and `login` against a remote registry
- **C-style preprocessor**: `#if`/`#elif`/`#else`/`#endif`, `#define`, `#undef`, `#error`, `#warning`, with automatic platform/architecture macros
- **LLVM-based code generation** with TBAA metadata, LLVM attribute inference, and optional DWARF debug info
- **Ahead-of-time compilation** to executables, static/shared libraries, LLVM IR, or assembly, plus an in-process **JIT** mode (`utopia run`)
- **Project builds** driven by a `build.yaml` manifest (targets: executable, library, shared library) with optional `build.utp` scripts executed via JIT, and a transitive build cache
- **Cross-compilation** support (`--target`, `--sysroot`, including Android via the NDK)
- **Language Server Protocol** implementation (hover, go-to-definition, completion, signature help, diagnostics, semantic tokens, formatting) and a built-in **code formatter**

### Standard library

- **`String`**: construction from any primitive, `+`/`==`/`!=` operators, indexing, `length()`, `c_str()`, `toInt()`/`toFloat()`, `clear()`, `push_back()`
- **`List<T>`**: dynamic generic list with `push()`, indexing, copy semantics, and array-literal initialization
- **`Console`**: `print`, `printLine`, `readLine`, `clear`; global `print(format, ...)` with `printf`-style formatting
- **`Math`**: numeric limits constants (`INT32_MAX`, `FLOAT64_MIN`, ...)
- **`Memory`**: `malloc`/`free` bindings and type reflection (`Type`, `MethodInfo`)
- **`System`**: `sleep`, `system` bindings
- **`Path`/`File`/`Directory`/`FileHandle`**: filesystem and file I/O utilities
- **`DynamicLibrary`**: runtime `dlopen`/`dlsym`-style FFI
- **Smart pointers** (`std.unique_ptr`, `std.shared_ptr`, `std.weak_ptr`, `std.make_unique`, `std.make_shared`)

## Installation

Utopia is currently tested and packaged for Fedora only.

1. Download the latest `.rpm` package from the repository's releases.
2. Install it with your package manager, for example:

```sh
   sudo dnf install ./utopia-<version>.rpm
```

This installs the `utopia` compiler CLI along with the prelude, standard library, and build-script library used by the toolchain.

## Hello World

```utp
int main() {
  print("Hello, World!\n");
  return 0;
}
```

Given a project with a `build.yaml` manifest pointing at this source file, compile and run it with:

```sh
utopia path/to/project
```

See `examples/01_helloworld` for the full project layout, including its `build.yaml`.

## Examples

The `examples/` directory contains small, self-contained programs demonstrating individual language features, including:

- Hello world and basic I/O
- String concatenation and operator overloading
- Enums and `switch` statements
- Recursion
- Classes and object lifetime
- Dynamic memory allocation
- Functions, named parameters, and default arguments
- Generics/templates
- Custom annotations
- Smart pointers with Rust-style auto-deref (`unique_ptr`, `shared_ptr`, `weak_ptr`)
- Dart-style lambdas with inferred signatures (`() => 5`, `(x) => x * 2`)

Each example is a standalone project with its own `build.yaml` manifest.

## Documentation

The `docs/` directory contains the full language documentation, organized like the books of other systems languages:

- **Getting Started** — installation, your first program, and project layout
- **Language Guide** — types, variables, functions, control flow, records, OOP, generics, operators, memory management and smart pointers, modules, preprocessor, and annotations
- **Standard Library** — `String`, `List`, `Console`, `Math`, `Memory`, `System`, I/O, FFI, and smart pointers
- **Tooling** — compiler CLI, build system, JIT, LSP, formatter, and cross-compilation targets
- **Advanced Topics** — LLVM code generation, TBAA, DWARF debug info, and intrinsics

Start at [docs/README.md](docs/README.md). The `examples/` directory also provides runnable, self-contained sample programs.

## Building from Source

Requirements:

- CMake 3.20+
- A C++23-capable compiler
- LLVM (development package, matching the version configured via `find_package(LLVM)`)

Build steps:

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --parallel
```

This builds `utopia_core` (the compiler library) and the `utopia` CLI tool under `tools/utopia`. Useful CMake options:

- `-DBUILD_TESTS=ON` to build the test suite (`tests/`)
- `-DENABLE_SANITIZERS=OFF` to disable UBSan (enabled by default)

Repository layout:

- `src/`, `include/` — compiler library (lexer, parser, semantic analysis, LLVM codegen)
- `tools/utopia/` — command-line driver
- `libs/` — prelude, standard library, and build-script library shipped with the toolchain
- `examples/` — sample programs (including `10_smart_pointers`)
- `tests/` — test suite
- `docs/` — full language documentation

## Project Status

Utopia is under active, exploratory development. The language and its standard library are subject to change without notice. It is not recommended for production use.