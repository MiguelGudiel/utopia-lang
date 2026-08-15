# Compiler CLI

The `utopia` command drives compilation, execution, formatting, and package management.

## Commands

| Command | Description |
| --- | --- |
| `build` | Compile a project (or a single file) |
| `run` | Compile and execute via the in-process JIT |
| `fmt` | Format source files in place |
| `yip` | Package manager (`get`, `add`, `publish`, `login`) |
| `--help` | Show usage |

## Global options

| Option | Description |
| --- | --- |
| `--emit-llvm` | Emit `.ll` IR files next to object files |
| `--emit-asm` | Emit `.s` assembly files |
| `--jit` | Force JIT execution (default for `run`) |
| `-g, --debug` | Include DWARF debug symbols |
| `-O<level>` | Optimization level (0–3) |
| `-D<macro>` | Define a preprocessor macro |
| `--target <triple>` | Target triple (e.g. `x86_64-pc-windows-msvc`) |
| `--sysroot <path>` | System root (e.g. Android NDK) |

## Examples

```sh
# Build an executable
utopia build path/to/project

# Build with debug info, optimizations, and IR/asm output
utopia build path/to/project -g -O3 --emit-llvm --emit-asm

# Compile and run through the JIT
utopia run path/to/project

# Format a single file or a project's sources
utopia fmt src/main.utp

# Compile a single file (must still live in a project)
utopia build src/main.utp
```

## Exit codes and output

- Successful builds print `[Build Success]` and the output path.
- Semantic or syntax errors print file:line:column diagnostics and exit non-zero.
- `utopia build` on a project produces artifacts under `build/` (see [Project Layout](../getting-started/project-layout.md)).
