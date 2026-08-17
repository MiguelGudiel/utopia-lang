# Cross-Compilation

Utopia targets arbitrary LLVM-supported triples.

## Target selection

```sh
utopia build path/to/project --target x86_64-pc-windows-msvc
utopia build path/to/project --target aarch64-linux-android --sysroot /path/to/ndk
```

| Option | Purpose |
| --- | --- |
| `--target <triple>` | Target triple (LLVM format) |
| `--sysroot <path>` | System root for headers/libraries (e.g. Android NDK) |

The driver initializes all LLVM targets, so any installed backend can be selected.

## Platform macros

The preprocessor macros are derived from the target triple, so conditional code adapts automatically:

```utp
#if _WIN32
  // Windows-only code
#elif __ANDROID__
  // Android-only code
#elif __APPLE__
  // Apple-only code
#elif __linux__ || __gnu_linux__
  // Linux code
#endif
```

## Android

The toolchain auto-detects the NDK through the `ANDROID_NDK_HOME` environment variable and uses the NDK's `clang`/`lld` for linking. Platform-specific link flags (`-lpthread`, `-lrt`) are filtered out for Android targets.

## Linking

- Executables and shared libraries are linked with `clang` (or the NDK toolchain).
- Static libraries are archived with `llvm-ar`.
- The output suffix adapts to the target: `.exe`/`.dll` on Windows, `.dylib` on macOS, `.so` on Linux/Android.

## Limitations

- JIT mode rejects cross-compilation (it can only execute host code).
- Foreign libraries and system headers must be provided through `--sysroot` and `include_dirs`/`linker_flags`.
