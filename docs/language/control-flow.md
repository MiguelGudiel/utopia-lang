# Control Flow

## Conditionals

```utp
if (score >= 90) {
  print("A\n");
} else if (score >= 80) {
  print("B\n");
} else {
  print("C\n");
}
```

Statements can be used without braces.

## Loops

```utp
// while
int i = 0;
while (i < 10) {
  i++;
}

// for (C-style)
for (int j = 0; j < 10; j++) {
  print("%d ", j);
}
```

`break` exits the innermost loop or switch; `continue` skips to the next iteration. The compiler validates that `break`/`continue` appear inside a breakable construct.

## For-in (Dart-style)

`for (var x in expr)` iterates any value whose type provides the structural
iteration protocol: there is no `Iterable` hierarchy and no virtual
dispatch:

```utp
List<String> names = ["ada", "grace"];
for (var name in names) {
  print("%s\n", name.c_str());
}

Map<String, int> scores = {"ada": 9001, "grace": 42};
for (var k in scores) {          // iterating a Map yields its KEYS
  print("%s=%d\n", k.c_str(), scores[k]);
}
for (var e in scores.entries()) { // key/value pairs
  print("%s -> %d\n", e.key.c_str(), e.value);
}
```

### The protocol

`expr.iterator()` must return a value whose type provides:

```utp
bool moveNext();  // advances; false once exhausted
T&   current();   // the current element (valid after a successful moveNext)
```

`List<T>`, `Map<K, V>`, `HashMap<K, V>`, `SplayTreeMap<K, V>`,
`ListLiteralView<T>` and any user type defining the protocol are iterable.
Iterating a `Map`/`HashMap`/`SplayTreeMap` yields the keys; use
`map.entries()` for key/value pairs.

The protocol is purely structural (duck typing): a type is iterable exactly
when it declares `iterator()`; nothing is inherited, no vtable is involved,
and the cursors are tiny value types returned by value. All of the prelude's
cursors and their `moveNext()`/`current()` methods are `@inline`, so the
optimizer reduces every loop to the same machine code as a manual walk
(raw pointer walk for `List`, linked-list walk for `Map`, parent-pointer
tree walk for `SplayTreeMap`).

### Loop variable forms

| Form        | Semantics                                                        |
|-------------|------------------------------------------------------------------|
| `var x`     | copies the element each iteration (rebindable, Dart semantics)   |
| `final x`   | non-rebindable copy                                              |
| `var& x`    | binds a mutable reference to the element (no copy; writes through) |
| `final& x`  | binds a const reference to the element (no copy, read-only)      |
| `T x`       | explicit element type (implicitly converted each iteration)      |

```utp
List<int> nums = [10, 20, 30];
for (var& n in nums) {   // zero-cost in-place mutation
  n = n * 2;
}
```

### Array literals and fixed-size arrays

`T[N]` iterables (including `[...]` literals) lower to a plain index loop
over the array itself, with no view object created:

```utp
for (var x in [1, 2, 3, 4]) {
  print("%d ", x);        // 1 2 3 4
}
```

### Lifetime

The iterable expression is evaluated exactly once. For r-value iterables the
compiler materializes a stack temporary whose lifetime covers the whole
loop (C++ range-for semantics), so `for (var x in getList())` is safe.
The cursors are invalidated by mutations that move elements (insertions
that grow a `List`, insertions that grow a map's table, any
`SplayTreeMap`/`HashMap` mutation).

### Errors

- Iterating a type without `iterator()` is an error (`Cannot iterate a
  value of type 'X'`).
- `moveNext()` must return a boolean and `current()` a non-void type.
- `break`/`continue` work as in any other loop.

## Switch

```utp
enum AppState { Loading, Ready, Error }

void handle(AppState state) {
  switch (state) {
    case AppState.Loading:
      print("Loading...\n");
      break;
    case AppState.Ready:
      print("Ready!\n");
      break;
    case AppState.Error:
      print("Error\n");
      break;
    default:
      print("Unknown\n");
  }
}
```

- `case` values can be any constant expression.
- `default` is optional and must be unique.
- `break` is required to exit a case (statements do not fall through to the next label).

## Return

```utp
int max(int a, int b) {
  if (a > b) return a;
  return b;
}
```

The compiler performs control-flow analysis (`guaranteesReturn`) so that a function with a complete `if`/`else` pair, an exhaustive `switch` with `default`, or an infinite `while (true)` loop is not flagged as missing a return.

## Ternary operator

```utp
int status = isReady ? 1 : 0;
```

The ternary promotes both branches to a common type and is an l-value when both branches are l-values:

```utp
(cond ? a : b) = 42;
```

## Casts

Casts use the `as` operator (C-style casts are not supported):

```utp
int32 i = value as int32;
float64 d = length as float64;
```

User-defined conversions via single-argument constructors are also usable with `as`:

```utp
var s = 42 as String;
```

## Exceptions (try / catch / throw)

Exceptions follow the C++ model: any type can be thrown and caught, and a
catch clause matches the exact thrown type, one of its base classes, or any
interface it implements. Catch clauses are tried in order.

```utp
class InsufficientFunds {
  public int needed;
  InsufficientFunds(int needed) {
    this.needed = needed;
  }
  InsufficientFunds(const InsufficientFunds& other) {
    needed = other.needed;
  }
}

void withdraw(BankAccount* account, int amount) {
  if (amount > account.balance) {
    throw InsufficientFunds(amount - account.balance);
  }
  account.balance -= amount;
}

try {
  withdraw(acc, 500);
} catch (InsufficientFunds e) {
  print("missing %d\n", e.needed);
} catch (...) {
  print("other failure\n");
}
```

`catch (...)` matches every type. A catch clause may declare a binding
variable (`catch (InsufficientFunds e)`), optionally by reference
(`catch (String& s)`), in which case it refers directly to the thrown
object.

A bare `throw;` inside a catch clause rethrows the exception currently
being handled, preserving its dynamic type:

```utp
try {
  try {
    throw String("inner");
  } catch (String s) {
    throw; // rethrow to the outer handler
  }
} catch (String s) {
  print("outer: %s\n", s);
}
```

Destructors of live locals run while an exception propagates through their
scope, so RAII-style cleanup works across `throw` sites. Throwing or
catching a record with a custom destructor requires a copy constructor
(C++ semantics); destructors themselves cannot throw. `try`/`catch` is not
available inside `async` functions.

An exception with no matching handler terminates the program, printing
`Unhandled exception` to stderr.

## assert

`assert(expr)` aborts with the source location when the expression
evaluates to false. It compiles to a no-op when `NDEBUG` is defined
(e.g. `utopia build -DNDEBUG`).

```utp
assert(index < list.length());
```

## Source location intrinsics

`__FILE__` expands to the current file path and `__LINE__` to the current
line number, mirroring C/C++:

```utp
print("reached %s:%d\n", __FILE__, __LINE__);
```
