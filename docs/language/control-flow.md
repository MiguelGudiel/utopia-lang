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
