# Project Layout

A Utopia project is a directory containing a `build.yaml` manifest and one or more source directories.

## Minimal layout

```
my_game/
├── build.yaml
└── src/
    └── main.utp
```

## The `build.yaml` manifest

```yaml
project:
  name: "my_game"          # project name (used for output naming)
  version: "0.1.0"         # optional

build:
  target: executable        # executable | library (static) | shared_library
  output_dir: build         # optional, default "build"
  output_name: my_game      # optional, defaults to project name
  optimization: 3           # optional, -O level (0-3), defaults from CLI
  linker_flags: ["-lm"]     # optional extra linker flags

  source_dirs:
    - src/                  # directories scanned recursively for .utp files
  sources:
    - "main.utp"            # optional explicit file list

  include_dirs:
    - lib/                  # optional extra import search paths

dependencies:               # optional
  - name: utpsdl            # registry package (resolved via yip)
    version: "0.1.3"
    link: static            # static | shared
  - path: ../engine         # local path dependency
    link: static
```

### Build targets

| Target | Output | Description |
| --- | --- | --- |
| `executable` | `build/bin/<name>` | Native executable |
| `library` / `static_library` / `static` | `build/lib/lib<name>.a` | Static archive (via `llvm-ar`) |
| `shared_library` / `shared` | `build/lib/lib<name>.so` (`.dylib`/`.dll` per target) | Shared library |

## Source organization

- `source_dirs` are scanned **recursively**; every `.utp` file found is compiled as part of the translation unit set.
- `sources` adds individual files (relative to the manifest directory).
- Sub-projects can depend on each other with `dependencies: [{ path: ..., link: static }]`, producing a topologically ordered build.

## Build artifacts

```
build/
├── bin/        # executables and shared libraries
├── lib/        # static libraries
└── obj/        # per-module object files and emitted LLVM IR
```

The object cache is incremental: unchanged modules are not recompiled (see [Build System](../tooling/build-system.md)).

## Build scripts

Projects may optionally define a `build.utp` script that customizes the build with a Utopia-native API:

```utp
addLinkerFlag("-lm");
setOptLevel(3);
addDefine({name: "MY_FEATURE", isPublic: true});
```

See [Build System](../tooling/build-system.md) for the full API.
