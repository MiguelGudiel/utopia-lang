# Types

Utopia is statically typed with an explicit-width integer family, IEEE-754 floating point, and a canonical, deduplicated type system.

## Primitive types

| Type | Aliases | Width | Notes |
| --- | --- | --- | --- |
| `int8` | `char` | 8 bits | signed byte |
| `int16` | | 16 bits | |
| `int32` | `int` | 32 bits | default integer type |
| `int64` | `long` suffix (`1L`) | 64 bits | |
| `uint8` | | 8 bits | |
| `uint16` | | 16 bits | |
| `uint32` | `uint` | 32 bits | `1U` |
| `uint64` | | 64 bits | `1UL` |
| `usize` | | pointer-width | `1UZ` |
| `float32` | `float` | 32 bits | `1.0F` |
| `float64` | `double` | 64 bits | |
| `bool` | | 1 bit | `true` / `false` |
| `rune` | | 32 bits | Unicode code point, `U'A'`, `U'\u732b'` |
| `void` | | — | no value |

### Integer suffixes

```utp
var a = 42;        // int32
var b = 42L;       // int64
var c = 42U;       // uint32
var d = 42UL;      // uint64
var e = 42UZ;      // usize
var h = 0xFF;      // hexadecimal
var f = 1.5F;      // float32
var g = 1.5;       // float64
```

## `const`

Any type can be `const`-qualified. `const` propagates into the type system and is enforced on assignment:

```utp
const int maxItems = 100;
void read(const uint8* buffer);
```

## Compound types

### Pointers

```utp
int* p = new int(42);
p.x          // member access dereferences automatically (see below)
delete p;
```

Pointers, references, and r-value references are **transparent on member access**: `p.field` and `p.method()` work directly, without `->` or explicit dereference.

### References and r-value references

```utp
void increment(int& value) { value++; }        // l-value reference
void consume(String&& temp) { /* r-value */ }  // r-value reference
```

### Arrays

```utp
int[4] fixed;                       // fixed-size array (type comes first)
int[3] data = [1, 2, 3];            // array literal initialization
int* dyn = new int[size];           // dynamic array (heap)
delete[] dyn;
```

Arrays decay to pointers when passed to functions. Array literals also initialize the `List` type:

### Function types

The `Function` keyword builds a function-pointer type:

```utp
int apply(int Function(int, int) fn, int a, int b) {
  return fn(a, b);
}
```

### Type aliases

```utp
typedef Counter = int32;
Counter c = 0;
```

## Type promotion

Arithmetic between mixed-width integers promotes to the widest safe type; floating point dominates integers; `usize` promotes dynamically to 64-bit when combined with 64-bit types:

```utp
var r = 1 + 2L;      // int64
var s = 1 + 2.5;     // float64
```

## Type checking

The compiler performs full semantic analysis before code generation: unknown identifiers, private accesses, mismatched assignments, and missing returns are reported with precise source locations. Type errors are diagnosed at compile time, never at run time.
