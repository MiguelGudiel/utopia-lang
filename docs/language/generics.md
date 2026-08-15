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
- Instantiated names are mangled deterministically (e.g. `Box_int`, `std.unique_ptr_Widget`), including namespace qualification.

## Smart pointer generics

The standard library's smart pointers are generic, and their `operator*` participates in Rust-style auto-deref:

```utp
import "utopia:memory";
using std;

unique_ptr<Inner> up = make_unique<Inner>(new Inner(42));
up.sayHello();   // resolved through operator* automatically
```
