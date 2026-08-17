# Modules

Utopia organizes code into modules, namespaces, and packages. Every module sees the prelude automatically.

## Importing modules

```utp
import "utopia:memory";        // standard library module
import "package:engine/core"; // registry package
import "../lib/util.utp";      // relative path (extension optional)
```

`export` re-exports a module so its symbols become visible to downstream importers:

```utp
export "String.utp";
export "Core/List.utp";
```

### Import resolution

| Form | Resolves to |
| --- | --- |
| `"prelude"` | the language prelude |
| `"utopia:name"` | `libs/stdlib/lib/name.utp` |
| `"utopia:builder"` | the build-script API (only from `build.utp`) |
| `"package:name/sub"` | a `yip` registry package |
| relative path | a file relative to the importing file |

Modules are cached by canonical path; circular imports are detected and rejected.

## The prelude

The prelude is loaded automatically for every module (except itself) and re-exports:

- `String` (dynamic string)
- `List<T>` (generic dynamic array)
- `Console` and the global `print`
- `Math/Limits` constants
- `Memory/Core` (`malloc`, `free`, `Type` reflection)
- `System/OS` (`sleep`, `system`)

## Namespaces

```utp
namespace Geometry {
  struct Point {
    public int x;
    public int y;
  }

  float64 distance(Point a, Point b);
}

namespace Geometry.Shapes {   // nested / multi-part
  class Circle { ... }
}
```

Namespaces can be reopened, and a file may use a file-scoped namespace:

```utp
namespace App;   // all top-level declarations in this file belong to App
```

## `using`

`using` brings namespace members into scope, similar to C#'s `using`:

```utp
import "utopia:memory";
using Memory;

int main() {
  unique_ptr<Widget> u = make_unique<Widget>(new Widget());
  // ^ unique_ptr and make_unique resolved through 'Memory'
}
```

`using` is lexical — it applies to the enclosing scope/block.

## Visibility between modules

- Top-level declarations are visible to importing modules unless `private` (or `_`-prefixed).
- Private members of records are enforced across module boundaries.
- `@export` functions keep their un-mangled name for linking with external code.

## Package dependencies

Projects declare registry or path dependencies in `build.yaml`:

```yaml
dependencies:
  - name: utpsdl
    version: "0.1.3"
    link: static
  - path: ../engine
    link: static
```

Registry packages are fetched by the `yip` package manager into `~/.utopia/cache/yip/packages/` and imported with `package:name/...`. See [Package Manager](../tooling/package-manager.md).
