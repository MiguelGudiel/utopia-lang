# Object-Oriented Programming

Classes in Utopia support inheritance, interfaces, abstract types, and virtual dispatch with a full vtable implementation.

## Inheritance

Single inheritance via `extends`. The derived class inherits fields, methods, and can access the base through `super`:

```utp
class Animal {
  protected String name;

  public Animal(String name) {
    this.name = name;
  }

  @virtual public void speak() {
    print("...\n");
  }
}

class Dog extends Animal {
  public Dog(String name) : super(name) {}

  @override public void speak() {
    print("%s says woof!\n", this.name.c_str());
  }
}
```

### `super`

- `super(...)` in a constructor delegates to the base constructor.
- `super.method()` / `super.field` access base members, respecting base privacy.

## Interfaces

Interfaces are modeled as abstract classes implemented with `implements`. A class may implement multiple interfaces:

```utp
abstract class Drawable {
  public void draw();
}

abstract class Resizable {
  public void resize(int w, int h);
}

class Window implements Drawable, Resizable {
  @override public void draw() { /* ... */ }
  @override public void resize(int w, int h) { /* ... */ }
}
```

## Abstract classes and methods

`abstract` is only allowed on classes. Methods of an abstract class may be declared without a body (they become abstract automatically):

```utp
abstract class Shape {
  public float64 area();
}

class Circle extends Shape {
  private float64 r;
  public Circle(float64 r) { this.r = r; }
  @override public float64 area() => 3.14159265 * r * r;
}
```

Instantiating an abstract class is a compile error.

## Virtual dispatch

Virtual methods are declared with the `@virtual` annotation and overridden with `@override`:

```utp
class Renderer {
  @virtual public void beginFrame() { /* default */ }
}

class OpenGLRenderer extends Renderer {
  @override public void beginFrame() { /* GL-specific */ }
}
```

- A class becomes polymorphic (gains a vtable) when it has virtual methods, overrides, abstract methods, or implements interfaces.
- The vtable is a per-class global array of function pointers, written into the object's `vptr` by the constructor.
- Virtual calls resolve through `this->vptr[index]` at run time; non-virtual calls are direct.
- `@override` without a matching virtual base method is a compile error.

## Access control

| Modifier | Meaning |
| --- | --- |
| `public` | Accessible everywhere |
| `private` | Accessible only within the declaring record |
| `protected` | Accessible within the record and its derived classes |
| `_`-prefix | Implicitly private |

```utp
class BankAccount {
  private int64 balance;
  protected String owner;
  public void deposit(int64 amount) { this.balance += amount; }
}
```

## `const` methods

A method declared `const` cannot modify the receiver; `const` is part of the overload signature:

```utp
class Point {
  public int x;
  public int y;
  public const int dot(Point other) { return this.x * other.x + this.y * other.y; }
}
```

## Member access on pointers

Member access works directly on pointers, references, and smart pointers — the compiler inserts the dereference automatically:

```utp
Point* p = new Point(10, 20);
p.x = 30;            // (*p).x = 30
p.dot(p);            // (*p).dot(*p)
delete p;
```

## Visibility across modules

`private` members are enforced across module boundaries based on the declaring file; `@export`-ed functions are linkable without mangling.
