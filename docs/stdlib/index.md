# Standard Library Overview

Utopia ships two layers of standard library:

## The prelude

`libs/prelude/lib/`: loaded **automatically** into every module. Prelude symbols have **no namespace**; they are available globally without `using`. It re-exports:

| Module | Contents |
| --- | --- |
| `String.utp` | The `String` type |
| `Core/List.utp` | `List<T>`, `ListLiteralView<T>` |
| `Core/Map.utp` | `Map<K, V>` (LinkedHashMap), `MapLiteralView<K, V>`, `hash<T>` |
| `IO/Console.utp` | `Console` class and the global `print` |
| `Memory/Core.utp` | `malloc`/`free`, `Type` reflection, `sizeof`/`typeof` intrinsics |
| `Core/Duration.utp` | `Duration` (time spans) |
| `Async/Future.utp` | `Future<T>` and async support |

Only universally needed types stay in the prelude: String, List, Map, the
memory model and Console. Everything else (math, system, time, collections,
SIMD, ...) lives in the standard library below, so programs that do not use
those features do not pay the compile/link cost.

## The standard library

`libs/stdlib/lib/`: imported explicitly with `import "utopia:...";`. Each module declares its own domain namespace, so members are reached as `IO.Path`, `Memory.unique_ptr`, etc. (or brought in with `using IO;`):

| Module | Import | Namespace | Contents |
| --- | --- | --- | --- |
| `memory.utp` | `import "utopia:memory";` | `Memory` | `unique_ptr`, `shared_ptr`, `weak_ptr`, `make_unique`, `make_shared` |
| `ffi.utp` | `import "utopia:ffi";` | `FFI` | `DynamicLibrary` (dlopen/dlsym) |
| `io.utp` | `import "utopia:io";` | `IO` | `Path`, `File`, `Directory`, `FileHandle` |
| `math.utp` | `import "utopia:math";` | (global) | `Math` class, limits constants (imports `limits.utp`) |
| `limits.utp` | `import "utopia:limits";` | (global) | `INT32_MAX`, `FLOAT64_MAX`, ... constants |
| `system.utp` | `import "utopia:system";` | (global) | `Env` (variables, `args()`, `hostname`), `sleep`, `system`, `exit`, `abort`, `Process` |
| `time.utp` | `import "utopia:time";` | (global) | `Stopwatch`, `DateTime` (ISO 8601 parse, IANA zones) |
| `random.utp` | `import "utopia:random";` | (global) | `Random` (xorshift64*) |
| `utility.utp` | `import "utopia:utility";` | (global) | `optional<T>`, `pair<T, U>`, `min`/`max`/`clamp`/`swap` |
| `collections.utp` | `import "utopia:collections";` | (global) | `HashMap`, `SplayTreeMap`, `Set`, `Queue`/`Deque`, `Stack`, `PriorityQueue` |
| `simd.utp` | `import "utopia:simd";` | (global) | portable SIMD ops plus the x86/NEON layers |

## The builder library

`libs/builder/lib/builder.utp`: available **only inside `build.utp`** scripts (imported implicitly), providing the build configuration API:

```utp
addLinkerFlag("-lm");
addIncludeDir("third_party/include");
setOptLevel(3);
addDefine({name: "FEATURE_X", isPublic: true});
removeDefine("OLD_MACRO");
bool enabled = isDefined("FEATURE_X");
```

## Reading the sources

All library sources are plain Utopia code under `libs/` and serve as reference implementations:

- `libs/prelude/lib/String.utp`
- `libs/prelude/lib/Core/List.utp`
- `libs/stdlib/lib/memory.utp`: smart pointers
- `libs/stdlib/lib/io.utp`: filesystem API
