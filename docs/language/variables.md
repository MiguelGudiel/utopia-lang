# Variables

## Declarations

Utopia requires an explicit type for local and global variables, or the `var`/`const` keywords for inferred types.

```utp
int count = 10;
String name = "Utopia";
var total = 1 + 2;          // inferred int32
const version = "0.1.0";    // inferred constant String
```

## `var` and `const`

- `var` infers the initializer's type.
- `const` infers the type and makes the variable immutable.

```utp
var x = 42;         // int32
x = 43;             // OK
const y = 42;
// y = 43;          // error: cannot assign to a constant variable
```

## Fields and statics

Records declare fields with optional `public`/`private`/`protected` modifiers and `static` members:

```utp
class Counter {
  public static int instances = 0;   // static field (module-level storage)
  private int value = 0;             // instance field

  public Counter() {
    Counter.instances++;
  }
}
```

Names beginning with an underscore (`_`) are implicitly private:

```utp
class Foo {
  int _internal;   // private by convention
}
```

## Globals

Global variables are compiled to module-level storage and support both compile-time constants and mutable state:

```utp
int globalCounter = 0;
const float64 TAU = 6.28318530718;

void bump() {
  globalCounter++;
}
```

## Scope

Blocks introduce lexical scopes; shadowing of parameters is rejected:

```utp
void f() {
  int x = 1;
  {
    int y = 2;
    print("%d %d\n", x, y);
  }
  // y is out of scope here
}
```

## Uninitialized variables

Declaring a variable without an initializer leaves it uninitialized (matching C semantics) unless the type provides a default constructor:

```utp
int value;            // uninitialized int
String text;          // default-constructed String
```
