# Generics

Utopia supports generic (template) classes, structs, unions, functions, and methods with on-demand instantiation.

## Generic classes

```utp
class Box<T> {
  private T item;
  private bool isEmpty;

  public Box() {
    this.isEmpty = true;
  }

  public Box(T initialItem) {
    this.item = initialItem;
    this.isEmpty = false;
  }

  public void setItem(T newItem) {
    this.item = newItem;
    this.isEmpty = false;
  }

  public const T getItem() {
    return this.item;
  }
}

int main() {
  Box<int>* intBox = new Box<int>(42);
  Box<float64>* floatBox = new Box<float64>(3.14159);
  print("%d\n", intBox.getItem());
  delete intBox;
  delete floatBox;
  return 0;
}
```

## Generic functions

```utp
T findMax<T>(T a, T b) {
  if (a > b) return a;
  return b;
}

int main() {
  int highest = findMax<int>(100, 250);
  float64 f = findMax<float64>(1.5, 2.5);
  return 0;
}
```

## Generic methods

Methods can declare their own type parameters:

```utp
class Stack<T> {
  // ...
  public void swapWith<T2>(Stack<T2> other) {
    // ...
  }
}
```

## Template rules

- Template parameters are written with angle brackets: `class Foo<A, B>`, `void f<T>(T x)`.
- Nested angle brackets (`Foo<Bar<int>>`) are handled correctly by the lexer/parser (`>>` is split).
- Instantiation is lazy: a template is cloned and type-checked the first time it is used with concrete type arguments.
- The compiler maintains a per-module cache of instantiated templates, so repeated uses share a single instantiation.
- Instantiated names are mangled deterministically (e.g. `Box_int`, `Memory.unique_ptr_Widget`), including namespace qualification.

## Constraints: `T extends X`

A template parameter can declare a bound with the Dart syntax `T extends X`.
The bound is checked at instantiation time (compile-time only — zero runtime
cost) and enables two things inside the template body:

- **Class bounds** (`T extends Animal`) resolve member access on `T` against
  the bound (`x.speak()` works on `T extends Animal`), and `x is T` is erased
  to the bound, like Dart.
- **Pseudo-types** constrain the argument kind without any class hierarchy:

| Bound            | Admits                                              |
|------------------|-----------------------------------------------------|
| `T extends Object`  | any type (the default when no bound is written)   |
| `T extends Record`  | any `struct` / `class` / `union`                  |
| `T extends Number`  | any integer or floating-point type                |
| `T extends Integer` | any integer type                                  |
| `T extends FloatingPoint` | `float32` / `float64`                     |
| `T extends SomeClass` | any class that extends or implements `SomeClass` |

```utp
T maxOf<T extends Number>(T a, T b) {
  return a > b ? a : b;
}

class Animal {
  @virtual String sound() { return "?"; }
}
class Dog extends Animal {
  @override String sound() { return "woof"; }
}

class Kennel<T extends Animal> {
  T pet;
  Kennel(T p) { this.pet = p; }
  String greet() { return this.pet.sound(); }  // member access via the bound
}

int main() {
  print("%lld\n", maxOf(3, 5));          // 5
  Kennel<Dog> k = Kennel<Dog>(Dog());
  print("%s\n", k.greet().c_str());      // woof (virtual dispatch)
  return 0;
}
```

A constraint violation is a compile-time error:

```
Template argument 'String' for 'T' does not satisfy the constraint
'T extends Number' on template 'NumberBox'.
```

## Template specialization

Like C++, a template can be specialized for specific argument lists. No
`template<>` keyword is needed: a template list containing concrete types
declares a specialization of an already-declared primary template with the
same arity.

```utp
class Storage<T> {           // primary template
  T value;
  String describe() { return "generic"; }
}

class Storage<int32> {       // complete specialization
  int32 value;
  String describe() { return "int32-packed"; }
}

class Pair<A, B> {           // primary template
  A first; B second;
  String describe() { return "generic pair"; }
}

class Pair<A, int64> {       // partial specialization: any Pair with int64
  A first; int64 second;     // as its second type argument
  String describe() { return "pair over int64"; }
}
```

Resolution rules (mirroring C++):

- `Storage<String>` uses the primary; `Storage<int32>` uses the complete
  specialization. Complete specializations are preferred over partial ones.
- A partial specialization is chosen by deducing its parameters from the
  pattern: `Pair<String, int64>` matches `Pair<A, int64>` with `A = String`,
  while `Pair<String, String>` falls back to the primary.
- If more than one specialization matches, the use is ambiguous and the
  compiler reports an error.
- A specialization must follow its primary template and match its arity;
  specializations of non-templates are rejected.
- Specialized records keep their own layout, constructors and methods, so a
  complete specialization can store a different representation (e.g. a
  packed `int32` instead of a generic `T`).

## Smart pointer generics

The standard library's smart pointers are generic, and their `operator*` participates in Rust-style auto-deref:

```utp
import "utopia:memory";
using Memory;

unique_ptr<Inner> up = make_unique<Inner>(new Inner(42));
up.sayHello();   // resolved through operator* automatically
```
