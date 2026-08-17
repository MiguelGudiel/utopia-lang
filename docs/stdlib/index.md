# Standard Library Overview

Utopia ships two layers of standard library:

## The prelude

`libs/prelude/lib/` — loaded **automatically** into every module. Prelude symbols have **no namespace**; they are available globally without `using`. It re-exports:

| Module | Contents |
| --- | --- |
| `String.utp` | The `String` type |
| `Core/List.utp` | `List<T>`, `ListLiteralView<T>` |
| `Core/Map.utp` | `Map<K, V>` (LinkedHashMap), `MapLiteralView<K, V>`, `hash<T>` |
| `Core/HashMap.utp` | `HashMap<K, V>` (unordered hash table) |
| `Core/SplayTreeMap.utp` | `SplayTreeMap<K, V>` (sorted by key) |
| `IO/Console.utp` | `Console` class and the global `print` |
| `Math/Limits.utp` | Numeric limits constants |
| `Memory/Core.utp` | `malloc`/`free`, `Type` reflection, `sizeof`/`typeof` intrinsics |
| `System/OS.utp` | `sleep`, `system` |

## The standard library

`libs/stdlib/lib/` — imported explicitly with `import "utopia:...";`. Each module declares its own domain namespace, so members are reached as `IO.Path`, `Memory.unique_ptr`, etc. (or brought in with `using IO;`):

| Module | Import | Namespace | Contents |
| --- | --- | --- | --- |
| `memory.utp` | `import "utopia:memory";` | `Memory` | `unique_ptr`, `shared_ptr`, `weak_ptr`, `make_unique`, `make_shared` |
| `ffi.utp` | `import "utopia:ffi";` | `FFI` | `DynamicLibrary` (dlopen/dlsym) |
| `io.utp` | `import "utopia:io";` | `IO` | `Path`, `File`, `Directory`, `FileHandle` |

## The builder library

`libs/builder/lib/builder.utp` — available **only inside `build.utp`** scripts (imported implicitly), providing the build configuration API:

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
- `libs/stdlib/lib/memory.utp` — smart pointers
- `libs/stdlib/lib/io.utp` — filesystem API
