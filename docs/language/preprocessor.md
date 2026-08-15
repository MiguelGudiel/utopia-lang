# Preprocessor

Utopia ships a C-style preprocessor for conditional compilation and simple macros.

## Conditional compilation

```utp
#define WITH_DEBUG

#if WITH_DEBUG
  print("debug build\n");
#endif

#if defined(USE_OPENGL)
  // ...
#elif defined(USE_VULKAN)
  // ...
#else
  // ...
#endif
```

Supported directives:

| Directive | Purpose |
| --- | --- |
| `#if` / `#elif` / `#else` / `#endif` | Conditional compilation (nestable) |
| `#define` | Define a macro (presence is what matters for `#if`) |
| `#undef` | Remove a macro |
| `#error` | Abort compilation with a message |
| `#warning` | Emit a warning |

`#if` expressions support `!`, `&&`, `||`, `==`, `!=`, parentheses, identifiers (true when defined), and `true`/`false`.

## Platform macros

The compiler defines platform and architecture macros automatically:

**Operating system**

| Macro | Platforms |
| --- | --- |
| `_WIN32` | Windows |
| `__APPLE__` | macOS/iOS |
| `__ANDROID__` | Android |
| `__linux__`, `__gnu_linux__` | Linux |
| `__BSD__` | FreeBSD / NetBSD / OpenBSD |

**Architecture**

| Macro | Architectures |
| --- | --- |
| `x64`, `x86_64`, `__x86_64__` | x86-64 |
| `x86`, `__i386__` | x86 32-bit |
| `arm64`, `__aarch64__` | AArch64 |
| `arm`, `__arm__` | ARM 32-bit |

## User-defined macros

```sh
utopia build project -DUSE_OPENGL -DMY_FLAG=1
```

Build scripts can also define macros at build time:

```utp
addDefine({name: "FEATURE_X", isPublic: true});
addCacheDefine({name: "LOCAL_ONLY", defaultValue: false, isPublic: false});
```

See [Build System](../tooling/build-system.md) for the `build.utp` API.
