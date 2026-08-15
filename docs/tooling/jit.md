# JIT Mode

`utopia run` compiles the project and executes it in-process through LLVM's ORC JIT — no linker invocation, no executable file required.

```sh
utopia run path/to/project
```

## How it works

1. The project (and its dependencies) are compiled to LLVM IR as usual.
2. Modules are loaded into an `LLJIT` instance as thread-safe modules.
3. Module constructors (`llvm.global_ctors`) run on initialization.
4. The JIT resolves and calls `main`.

## Behavior notes

- Object files are still emitted to `build/obj/` even in JIT mode (useful for debugging).
- Cross-compilation is rejected in JIT mode — the JIT can only run code for the host machine.
- Global initialization and deinitialization are wired to the JITDylib lifecycle.

## Use cases

- Rapid iteration during development.
- Running `build.utp` scripts (the build system itself executes build scripts through the JIT).
- Embedding the compiler as a library (`utopia_core`) for tooling that needs to run Utopia code in-process.

## Example

```sh
utopia run examples/10_smart_pointers
```

prints the smart pointer demo without producing a binary.
