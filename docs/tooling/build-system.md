# Build System

Utopia projects are built from a `build.yaml` manifest, optionally extended by a `build.utp` script executed through the JIT.

## `build.yaml`

```yaml
project:
  name: "my_game"
  version: "0.1.0"

build:
  target: executable          # executable | library | shared_library
  output_dir: build
  output_name: my_game
  optimization: 3
  linker_flags: ["-lm"]

  source_dirs:
    - src/
  sources:
    - "main.utp"
  include_dirs:
    - lib/

dependencies:
  - name: utpsdl
    version: "0.1.3"
    link: static
  - path: ../engine
    link: static
```

### Key fields

| Field | Description |
| --- | --- |
| `project.name` | Project name (used for the default output name) |
| `build.target` | `executable`, `library`/`static_library`/`static`, `shared_library`/`shared` |
| `build.output_dir` | Artifact root (default `build`) |
| `build.output_name` | Overrides the output file name |
| `build.optimization` | Default `-O` level (overridable from the CLI) |
| `build.linker_flags` | Extra flags passed to the linker |
| `build.source_dirs` | Directories scanned recursively for `.utp` files |
| `build.sources` | Explicit source file list |
| `build.include_dirs` | Extra import search paths |
| `dependencies` | Registry packages (`name`+`version`) or local paths (`path`); `link: static` or `shared` |

## Build scripts (`build.utp`)

A project may include a `build.utp` script: Utopia code executed through the JIT before compilation. It customizes the build with the `utopia:builder` API:

```utp
addLinkerFlag("-lm");
addIncludeDir("third_party/include");
setOptLevel(3);

addDefine({name: "FEATURE_X", isPublic: true});
addCacheDefine({name: "LOCAL", defaultValue: false, isPublic: false});

if (isDefined("FEATURE_X")) {
  addLinkerFlag("-DX=1");
}

String root = MAIN_PROJECT_ROOT;
String out = OUTPUT_DIR;
```

### Builder API

| Function / constant | Description |
| --- | --- |
| `addLinkerFlag(String flag)` | Append a linker flag |
| `addIncludeDir(String dir)` | Append an include directory |
| `setOptLevel(int32 level)` | Set optimization (0–3) |
| `addDefine({String name, bool isPublic})` | Define a public/private macro |
| `removeDefine(String name)` | Remove a macro |
| `isDefined(String name) → bool` | Query a macro |
| `addCacheDefine({String name, bool defaultValue, bool isPublic})` | Define a cached macro |
| `MAIN_PROJECT_ROOT` | Root of the main project |
| `CURRENT_PROJECT_ROOT` | Root of the project being built |
| `OUTPUT_DIR` / `MAIN_OUTPUT_DIR` | Output directories |
| `TARGET_TRIPLE`, `TARGET_OS`, `TARGET_ARCH` | Target information |
| `BUILD_TYPE` | Build type |

## Build cache

The compiler maintains a per-module JSON cache keyed by timestamp and imports. Unchanged modules skip parsing, semantic analysis, and code generation entirely, which keeps incremental builds fast. The cache is invalidated transitively when any imported module changes.

## Dependency resolution order

1. Local path dependencies (`dependencies: [{ path: ... }]`) are built first, topologically.
2. Registry dependencies are resolved by `yip` and cached under `~/.utopia/cache/yip/packages/<name>/<version>/`.
3. Each dependency is itself a full project (own `build.yaml`, own cache), linked as static or shared.
