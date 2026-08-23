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

## Lambdas

Lambdas use Dart-style syntax and are typed from the function pointer they are
assigned to (or passed to). The return type is optional: when omitted, the
compiler infers it from the expected signature, or from the body when there is
no signature context.

```utp
int apply(int Function(int) fn, int value) => fn(value);

int main() {
  /* An immediately-invoked lambda; the return type is inferred from the
   * expression body. */
  int myNumber = (() => 5)();
  print("%d\n", myNumber);               // 5

  /* The parentheses are exactly like a function call: positional arguments,
   * with or without explicit types. */
  int sum = ((int a, int b) => a + b)(1, 2);
  print("%d\n", sum);                    // 3

  /* Untyped parameters are inferred from the target function pointer. */
  int Function(int) twice = (x) => x * 2;
  print("%d\n", twice(21));              // 42

  /* Block bodies are supported. */
  int Function(int) square = (int x) { return x * x; };
  print("%d\n", square(7));              // 49

  /* The return type may be written explicitly. */
  int Function(int) addHundred = int (x) => x + 100;
  print("%d\n", addHundred(5));          // 105

  /* A lambda whose body has no return (or only bare `return;`) is void. */
  void Function() greet = () { print("Hello!\n"); };
  greet();

  /* Lambdas are closures: they can capture enclosing locals and
   * parameters. The captured values are copied (by value) into a
   * reference-counted environment at creation; writes inside the closure
   * do not propagate back to the outer variable. */
  int factor = 3;
  int Function(int) f = (x) => x * factor;
  print("%d\n", f(21));                    // 63

  /* The async callback APIs (then, catchError, timeout, sync, delayed,
   * microtask, whenComplete, runOnThread) retain the environment while a
   * callback is registered, so a closure outlives its creating frame. */
  int base = 100;
  int t = await Future.delayed<int>(10, () => base + 1);
  return 0;
}
```

Lambdas passed as call arguments have their parameters and return type checked
against the parameter's function pointer signature:

```utp
print("%d\n", apply((y) => y * 10, 4));   // 40
```

A capturing lambda (or a variable holding one) can only be passed to the
closure-aware async callback APIs above: a plain function-pointer parameter
would receive the environment pointer and call it as a function address, so
Sema rejects that combination.

Lambda parameters support everything regular function parameters do: default
values, named parameters inside braces, and the `required` modifier.

```utp
int Function(int, int) f = (int a, int b = 10) => a + b;
```

Error cases:

- A lambda with untyped parameters must have a target signature; otherwise the
  compiler reports `Cannot infer lambda parameter types`.
- The lambda's parameter count and types must match the target signature.
- If the target signature has a non-`void` return type, the lambda body must
  return a value convertible to it.

## `const` methods

Methods can be declared `const`, which participates in overloading and is part of the method's signature:

```utp
class Box<T> {
  private T item;
  public const T getItem() { return this.item; }
}
```
