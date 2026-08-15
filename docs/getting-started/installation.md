# Installation

Utopia ships as a compiler CLI (`utopia`) together with the prelude, standard library, and build-script library.

## Package installation

Utopia is currently tested and packaged for Fedora Linux.

```sh
sudo dnf install ./utopia-<version>.rpm
```

This installs the `utopia` compiler binary and the standard library under the installation prefix:

```
<prefix>/bin/utopia
<prefix>/lib/utopia/prelude/lib/
<prefix>/lib/utopia/stdlib/lib/
<prefix>/lib/utopia/builder/lib/
```

## Building from source

### Requirements

- CMake 3.20+
- A C++23-capable compiler (GCC 13+, Clang 16+)
- LLVM development package (matching the version configured via `find_package(LLVM)`)

### Steps

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --parallel
```

This produces:

- `libutopia_core.a` — the compiler library (lexer, parser, semantic analysis, LLVM codegen)
- `tools/utopia/utopia` — the `utopia` CLI
- `tools/lsp/utopia_lsp` — the language server

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `BUILD_TESTS` | `OFF` | Build the test suite (`tests/`) |
| `ENABLE_SANITIZERS` | `ON` | Enable UBSan during development builds |

## Verifying the installation

```sh
utopia --help
```

Then compile a project:

```sh
utopia build path/to/project
```

See [Hello, World!](hello-world.md) for a complete first program.
