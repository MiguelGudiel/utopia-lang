# Const expressions and canonicalization

Utopia has Dart-style `const` expressions: values computed at compile time,
stored in static read-only storage, and **canonicalized**: every use of the
same constant expression refers to the *same* instance. There is no runtime
allocation and no constructor call at runtime for `const` values.

A `const` value is never constructed at runtime: the compiler evaluates the
expression during compilation, serializes it into a canonical key, and
emits a single static object. Two `const` expressions with identical
values are the same object (`==` on their pointers is `true`), which is
what Dart calls canonicalization.

## Two meanings of `const`

`const` appears in three positions with related but distinct meanings:

| Position | Meaning |
| --- | --- |
| `const Type name = expr;` | Declares a variable whose type is `const`-qualified and whose initializer must be a constant expression |
| `const expr` (expression position) | Evaluates `expr` at compile time and yields a pointer to the canonical static instance (for objects) or the constant value (for primitives) |
| `const Type(...)` (constructor) | Declares a **const constructor**: callable in const contexts; its calls produce canonical instances |

A `const` variable is a constant expression; a `final` variable is **not**
(Dart rule): only `const` names can be referenced inside other constant
expressions.

## Canonical objects

```utp
class Point {
  final int32 x;
  final int32 y;
  const Point(this.x, this.y) {}
}

int main() {
  const Point* a = const Point(1, 2);
  const Point* b = const Point(1, 2);
  const Point* c = const Point(3, 4);

  print("%d\n", a == b);   // 1: same canonical instance
  print("%d\n", a == c);   // 0: different value, different instance
  return 0;
}
```

- Like `new`, a const object creation yields a **pointer** to the object
  (Utopia's class model is pointer-based); the object lives in static
  read-only storage and is **immortal**: its destructor never runs and it
  must never be `delete`d.
- The canonical key is built from the class name (template arguments
  included), the constructor name, the parameter signature and the
  evaluated argument values, so two calls through **different named
  constructors** stay distinct even with identical arguments, and two calls
  with the same arguments through the same constructor are one instance.
- The static globals use deterministic names with `linkonce_odr` linkage, so
  the linker folds identical const objects across modules into a single
  address.

### Implicit const (Dart rule)

Inside a const context, nested constructor calls are implicitly const, no
`const` keyword needed:

```utp
class Line {
  final Point* a;
  final Point* b;
  const Line(this.a, this.b) {}
}

const Line* l1 = const Line(Point(1, 2), Point(3, 4));
const Line* l2 = const Line(Point(1, 2), Point(3, 4));
// l1 == l2, the nested Points are canonical instances, so the Lines
// canonicalize to one object.
```

Nested canonical objects are referenced through pointers: a field holding
another const object is declared `Point*` (the const creation yields a
pointer to the canonical instance).

## Const constructors

A constructor is a const constructor when it is declared `const`:

```utp
class Point {
  final int32 x;
  final int32 y;
  const Point(this.x, this.y) {}
  const Point.origin() : this.x = 0, this.y = 0 {}   // named const ctor
}
```

Rules (checked at compile time, Dart rules):

- The constructor body must be **empty**; all initialization happens
  through `this.x` parameters and `: this.x = expr` initializer-list
  entries.
- Every instance field of the class must be **`final`**, and declaration
  initializers of those fields must themselves be constant expressions.
- `const` constructors cannot be `async` or variadic.
- They are supported on **classes** (structs are value types).
- The super-constructor chain of a const constructor must be const.
- A const constructor call outside a const context still yields the
  canonical static instance (`const Point(1, 2)` in a normal statement
  works; it is just a compile-time object).

## Constant expressions

The following are valid constant expressions:

- Number, boolean, string, character, rune and `null` literals; enum
  members.
- `const` variables (and constructor parameters inside a const
  initializer). `final` variables are **not** constant.
- Arithmetic (`+ - * / % & | ^ << >>`), comparisons (`== != < <= > >=`)
  and logical operators (`&& ||`, with short-circuit) over constant
  operands; string concatenation with `+`.
- Ternary operators whose condition is a constant.
- Numeric casts (including int/float conversions) and pointer casts
  between const-object types.
- Field access on a canonical const object (`constPoint.x`), including
  through `const`-qualified pointer variables (`const Point* const p`).
- Const constructor calls (`const Point(1, 2)` or implicitly-const nested
  calls).
- `const [...]` array literals: every element must be constant; identical
  content canonicalizes to one static backing array (object elements are
  canonical instances, so the serialized key is exact).

A `const` variable whose initializer is not a constant expression is an
error:

```utp
int32 value = compute();          // runtime call
const int32 c = compute();        // error: not a constant expression
```

## `Memory.isConst`

The prelude exposes a runtime check for canonical const objects:

```utp
import "utopia:memory";
using Memory;

public bool isConst(void* ptr);
```

`isConst(ptr)` returns `true` when `ptr` points at a canonical const
object (compile-time static read-only storage). Such objects are immortal:
their destructors never run and they must never be freed. Frameworks that
receive objects by pointer (e.g. a UI framework handed a widget) can query
this before deleting: skip the `delete` when `isConst` is true, otherwise
free normally. The check is O(N) over the program's const objects, which is
negligible for real-world counts.

```utp
Point* p = const Point(1, 2);
print("%d\n", Memory.isConst(p));      // 1

Point* heap = new Point(9, 9);
print("%d\n", Memory.isConst(heap));   // 0
delete heap;                           // heap objects free normally
```
