# Functions

## Basic functions

```utp
int add(int a, int b) {
  return a + b;
}

// Expression-bodied
int square(int x) => x * x;

// Void
void greet() {
  print("Hello!\n");
}
```

Every function must end with a return when it declares a non-`void` return type; the compiler analyzes control flow to prove this.

## Parameters

### Positional parameters

```utp
int clamp(int value, int min, int max) {
  if (value < min) return min;
  if (value > max) return max;
  return value;
}
```

### Optional (default) parameters

```utp
void configure(String host, int port = 8080) {
  print("Connecting to %s:%d\n", host.c_str(), port);
}
```

### Named parameters

Named parameters are declared inside braces and passed by name. They may have defaults and can be marked `required`:

```utp
void configureServer(String host, {int port = 8080, required bool useSSL}) {
  // ...
}

int main() {
  configureServer("api.example.org", useSSL: true);
  configureServer("localhost", port: 3000, useSSL: false);
  return 0;
}
```

Rules:

- `required` is only valid on named parameters.
- A `required` parameter cannot have a default value.
- Positional arguments cannot appear after named arguments.
- Missing `required` named parameters are compile errors.

### Variadic parameters

A trailing `...` accepts any number of extra arguments (lowered to C varargs):

```utp
int sumAll(int count, ...) {
  // ...
}
```

Floats are automatically promoted to `double` in variadic positions, matching C.

## Overloading

Functions and methods overload by signature (parameter types, plus `const` for methods):

```utp
int sum(int a, int b)      { return a + b; }
float sum(float a, float b) { return a + b; }

int main() {
  print("%d\n", sum(1, 2));      // int overload
  print("%f\n", sum(1.0F, 2.0F)); // float overload
  return 0;
}
```

Overload resolution scores candidates by conversion quality (exact match, reference binding, r-value preference, ...).

## The `main` entry point

```utp
int main() { ... }
// or
int main(int32 argc, uint8** argv) { ... }
// or
void main() { ... }
```

The JIT and linker both resolve `main`.

## Function pointers

```utp
int twice(int x) => x * 2;

int apply(int Function(int) fn, int value) {
  return fn(value);
}

int main() {
  print("%d\n", apply(twice, 21));   // 42
  return 0;
}
```

## `const` methods

Methods can be declared `const`, which participates in overloading and is part of the method's signature:

```utp
class Box<T> {
  private T item;
  public const T getItem() { return this.item; }
}
```
