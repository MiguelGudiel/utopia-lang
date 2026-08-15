# Utopia Documentation

Welcome to the Utopia language documentation. Utopia is a low-level, statically typed programming language with a Dart-inspired syntax and a C/C++-style memory and execution model. It compiles ahead-of-time to native code through LLVM.

This book is organized like the documentation of other systems languages, so you can read it front-to-back or jump straight to the topic you need.

## Table of contents

### Getting Started

| Chapter | Description |
| --- | --- |
| [Installation](getting-started/installation.md) | Installing the compiler toolchain |
| [Hello, World!](getting-started/hello-world.md) | Your first Utopia program |
| [Project Layout](getting-started/project-layout.md) | The `build.yaml` manifest and how projects are organized |

### Language Guide

| Chapter | Description |
| --- | --- |
| [Types](language/types.md) | Primitive types, literals, and compound types |
| [Variables](language/variables.md) | Declarations, `var`/`const` inference, and scope |
| [Functions](language/functions.md) | Parameters, defaults, named/required parameters, variadics, overloading |
| [Control Flow](language/control-flow.md) | `if`, `while`, `for`, `switch`, `break`, `continue`, `return` |
| [Records](language/records.md) | `struct`, `class`, and `union` declarations |
| [Enums](language/enums.md) | Typed enumerations |
| [Object-Oriented Programming](language/oop.md) | Inheritance, interfaces, abstraction, polymorphism, access control |
| [Generics](language/generics.md) | Template classes, functions, and methods |
| [Operators](language/operators.md) | Built-in operators and operator overloading |
| [Memory Management](language/memory.md) | `new`/`delete`, RAII, pointers, smart pointers, auto-deref |
| [Modules](language/modules.md) | `import`/`export`, namespaces, `using`, the prelude |
| [Preprocessor](language/preprocessor.md) | `#if`, `#define`, and platform macros |
| [Annotations](language/annotations.md) | Built-in and user-defined annotations |

### Standard Library

| Chapter | Description |
| --- | --- |
| [Overview](stdlib/index.md) | The prelude and `utopia:` modules |
| [String](stdlib/string.md) | Dynamic string type |
| [List](stdlib/list.md) | Generic dynamic array |
| [Console](stdlib/console.md) | Terminal I/O and formatted printing |
| [Math](stdlib/math.md) | Numeric limits |
| [Memory & Reflection](stdlib/memory.md) | `malloc`/`free` and `sizeof`/`typeof` |
| [System](stdlib/system.md) | OS bindings (`sleep`, `system`) |
| [I/O](stdlib/io.md) | `Path`, `File`, `Directory`, `FileHandle` |
| [FFI](stdlib/ffi.md) | Dynamic library loading and symbol resolution |
| [Smart Pointers](stdlib/smart-pointers.md) | `unique_ptr`, `shared_ptr`, `weak_ptr`, `make_unique`, `make_shared` |

### Tooling

| Chapter | Description |
| --- | --- |
| [Compiler CLI](tooling/compiler-cli.md) | `build`, `run`, `fmt`, `yip` and CLI flags |
| [Build System](tooling/build-system.md) | `build.yaml` and `build.utp` scripts |
| [Package Manager (yip)](tooling/package-manager.md) | Dependencies and publishing |
| [JIT Mode](tooling/jit.md) | In-process execution via `utopia run` |
| [LSP](tooling/lsp.md) | Editor integration and language features |
| [Formatter](tooling/formatter.md) | Code formatting |
| [Cross-Compilation](tooling/cross-compilation.md) | Targets, sysroots, and Android |

### Advanced Topics

| Chapter | Description |
| --- | --- |
| [LLVM Code Generation](advanced/llvm-codegen.md) | The compilation pipeline and attribute inference |
| [TBAA Metadata](advanced/tbaa.md) | Type-based alias analysis |
| [Debug Info](advanced/debug-info.md) | DWARF emission |
| [Intrinsics](advanced/intrinsics.md) | `sizeof`, `typeof`, and type reflection |

## Conventions

Code blocks use the `utp` language tag. Shell commands use `sh`. All examples are self-contained and mirror the runnable programs in the repository's `examples/` directory.

The compiler is written in C++23 on top of LLVM and lives in `src/`; the standard library source is in `libs/`.
