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
