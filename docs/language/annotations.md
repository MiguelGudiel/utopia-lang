# Annotations

Annotations provide declarative metadata that changes compilation behavior, linking, and code generation. They are written with `@name` before a declaration.

## Built-in annotations

### Linking & exports

| Annotation | Effect |
| --- | --- |
| `@extern` | Declares an external symbol. Optional args: `@extern("alias")` or `@extern("alias", "stdcall"|"cdecl"|"fastcall")` |
| `@export` | Keeps the un-mangled name (symbol exported as-is) |
| `@weak` | Emits the symbol with weak linkage |
| `@intrinsic("name")` | Declares a compiler intrinsic (no body) |

```utp
@extern("printf")
int32 print(const uint8* format, ...);

@extern("malloc")
void* malloc(usize size);
```

### Optimization hints

| Annotation | Effect |
| --- | --- |
| `@inline` | LLVM `InlineHint` attribute |
| `@forceInline` | LLVM `AlwaysInline` attribute |
| `@readnone` | Function reads/writes no memory |
| `@readonly` | Function only reads memory |
| `@nosync` | No synchronization |
| `@nofree` | Does not free memory |
| `@willreturn` | Guaranteed to return |
| `@mustprogress` | Guaranteed to make progress |
| `@nocapture` | Parameter is not captured |
| `@nonnull` | Parameter/return is non-null |
| `@dereferenceable(N)` | Pointer is dereferenceable for N bytes |

Many of these attributes are also **inferred automatically** by the effect analyzer for non-`@extern` functions.

### Layout

| Annotation | Effect |
| --- | --- |
| `@align(N)` | Record alignment (must be a power of two) |
| `@packed` | Packed record layout (no padding) |

### Diagnostics

| Annotation | Effect |
| --- | --- |
| `@nodiscard` | Warning when the return value is ignored |
| `@deprecated` | Warning on use. Optional message: `@deprecated("use bar() instead")` |

### Polymorphism

| Annotation | Effect |
| --- | --- |
| `@virtual` | Declares a virtual method (vtable slot) |
| `@override` | Overrides a base virtual method (validated by the compiler) |

```utp
class Renderer {
  @virtual public void beginFrame() { }
}

class GLRenderer extends Renderer {
  @override public void beginFrame() { /* ... */ }
}
```

## User-defined annotations

Annotation classes are declared with `annotation class` and require a `const` constructor. Fields capture arguments:

```utp
annotation class Route {
  public String path;
  public String method;

  const Route(String path, String method) {
    this.path = path;
    this.method = method;
  }
}

annotation class Serializable {
  const Serializable() {}
}
```

Usage:

```utp
@Serializable
@Route("/api/v1/users", "GET")
class UsersEndpoint { ... }
```

Annotation arguments can be string literals, numbers, and other constant expressions. The LSP surfaces user-defined annotations in completion.
