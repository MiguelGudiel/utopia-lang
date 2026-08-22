# Records: struct, class, and union

Records bundle data and behavior. Utopia provides three record kinds:

- **`struct`**: a value-like aggregate with optional methods and constructors.
- **`class`**: an object-oriented type supporting inheritance, interfaces, and polymorphism.
- **`union`**: an aggregate whose fields overlap in memory (like C unions).

All three support fields, multiple constructors, destructors, methods, static members, access modifiers, and templates.

## Structs

```utp
struct Point {
  public int x;
  public int y;

  public Point(int x, int y) {
    this.x = x;
    this.y = y;
  }

  public void print() {
    print("(%d, %d)\n", this.x, this.y);
  }
}
```

## Classes

```utp
class Account {
  private String owner;
  private int64 balance;

  public Account(String owner, int64 balance) {
    this.owner = owner;
    this.balance = balance;
  }

  public void deposit(int64 amount) {
    this.balance += amount;
  }

  public const int64 getBalance() {
    return this.balance;
  }

  public ~Account() {
    print("Closing account of %s\n", this.owner.c_str());
  }
}
```

Classes are heap-allocated with `new` and released with `delete`:

```utp
int main() {
  Account* acc = new Account("Alice", 1000);
  acc.deposit(500);
  print("%d\n", acc.getBalance());
  delete acc;
  return 0;
}
```

## Unions

```utp
union Value {
  public int32 asInt;
  public float32 asFloat;
  public uint8 bytes[4];
}
```

## Constructors

- Multiple constructors may be defined; they overload by signature.
- A default constructor and an empty destructor are generated implicitly when absent.
- Constructors may delegate to the base class with `: super(...)`.
- Annotation classes require a `const` constructor.

```utp
class Point3D extends Point {
  public int z;

  public Point3D(int x, int y, int z) : super(x, y) {
    this.z = z;
  }
}
```

## Destructors (RAII)

Destructors run deterministically:

- at the end of the enclosing scope (for value/stack-allocated records),
- on early `return`,
- on loop exits,
- when `delete` is called on a heap object,

and the compiler ensures copy/move semantics stay consistent for records with non-trivial destructors:

```utp
class Resource {
  private int* handle;
  public Resource() { this.handle = new int(0); }
  public ~Resource() { if (this.handle != null) delete this.handle; }
  // A copy constructor is required for by-value copies;
  // implicit copying of records with custom destructors is rejected.
}
```

## Static members

```utp
class MathUtils {
  public static int square(int x) => x * x;
  public static int callCount = 0;
}

int main() {
  MathUtils.callCount++;
  print("%d\n", MathUtils.square(9));
  return 0;
}
```

## Forward (opaque) declarations

```utp
class Node;          // opaque type, useful for recursive structures
```
