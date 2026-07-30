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

Based on the current compiler and standard library implementation:

- Static typing with explicit-width integers (`int8`–`int64`, `uint8`–`uint64`), `float32`/`float64`, `bool`, `char`, `rune`, and `void`
- Structs and classes with constructors, destructors, methods, and static members
- Access control via `public`/`private` modifiers
- Operator overloading (`operator+`, `operator==`, `operator[]`, etc.)
- Basic generics for functions, classes, and methods (e.g. `class Box<T>`)
- Enums with an explicit underlying integer type
- Pointers, references, and r-value references
- Manual memory management via `new`/`delete`, including array allocation
- Fixed-size arrays and array literals
- Named and optional parameters, including a `required` modifier for named parameters
- Expression-bodied functions (`=>`)
- An annotation system (`@extern`, `@export`, `@align`, `@packed`, and user-defined annotation classes)
- A module system based on `import` statements, with a small prelude (`String`, `Console`, memory and OS bindings)
- A C-style preprocessor (`#if`/`#elif`/`#else`/`#endif`, `#define`, `#undef`) with built-in platform/architecture macros
- LLVM-based code generation with TBAA metadata and optional DWARF debug info
- Ahead-of-time compilation to executables, shared libraries, or LLVM IR/assembly, as well as an in-process JIT execution mode
- Project builds driven by a `build.yaml` manifest, with optional `build.utp` build scripts executed via JIT

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

Each example is a standalone project with its own `build.yaml` manifest.

## Documentation

A `docs/` directory exists in this repository for language documentation. It is still under development; the primary way to learn the language today is through the `examples/` directory and the standard library sources under `libs/`.

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
- `examples/` — sample programs
- `tests/` — test suite
- `docs/` — documentation (in progress)

## Project Status

Utopia is under active, exploratory development. The language and its standard library are subject to change without notice. It is not recommended for production use.