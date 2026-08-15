# Standard Library Overview

Utopia ships two layers of standard library:

## The prelude

`libs/prelude/lib/` — loaded **automatically** into every module. It re-exports:

| Module | Contents |
| --- | --- |
| `String.utp` | The `String` type |
| `Core/List.utp` | `List<T>`, `ListLiteralView<T>` |
| `IO/Console.utp` | `Console` class and the global `print` |
| `Math/Limits.utp` | Numeric limits constants |
| `Memory/Core.utp` | `malloc`/`free`, `Type` reflection, `sizeof`/`typeof` intrinsics |
| `System/OS.utp` | `sleep`, `system` |

## The standard library

`libs/stdlib/lib/` — imported explicitly with `import "utopia:...";`:

| Module | Import | Contents |
| --- | --- | --- |
| `memory.utp` | `import "utopia:memory";` | `std.unique_ptr`, `std.shared_ptr`, `std.weak_ptr`, `std.make_unique`, `std.make_shared` |
| `ffi.utp` | `import "utopia:ffi";` | `DynamicLibrary` (dlopen/dlsym) |
| `io.utp` | `import "utopia:io";` | `Path`, `File`, `Directory`, `FileHandle` |
| `test.utp` | `import "utopia:test";` | Example overload set |

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
