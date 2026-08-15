# Memory Management

Utopia follows the C/C++ memory model: manual allocation, pointers, and deterministic destruction.

## Manual allocation

```utp
int* p = new int(42);        // single object
delete p;

int* arr = new int[10];      // array
delete[] arr;
```

- `new T(args...)` allocates, default-initializes, and runs the matching constructor.
- `new T[n]` over-allocates by 8 bytes to store the element count; `delete[]` reads it back and runs destructors in reverse order before freeing.
- `delete` performs a null check, runs the destructor, then frees.
- The prelude binds the underlying `malloc`/`free` (`Memory/Core.utp`).

## RAII

Records with destructors get deterministic cleanup:

```utp
class Guard {
  private int* handle;
  public Guard() { this.handle = new int(0); }
  public ~Guard() { if (this.handle != null) delete this.handle; }
}

void f() {
  Guard g;                 // constructed
  if (something) return;   // g is destroyed here too
  // ...                  // g is destroyed at scope exit
}
```

The compiler emits cleanups for:

- local variables at scope exit,
- temporaries and function-call results,
- loop bodies,
- function parameters passed by value (with proper copy/move materialization),
- early returns.

Records with custom destructors cannot be implicitly copied — a copy constructor or move constructor must be provided, preventing accidental double frees at compile time.

## Copy and move semantics

```utp
class unique_ptr<T> {
  private T* _ptr;

  public unique_ptr(unique_ptr<T>&& other) {   // move constructor
    this._ptr = other._ptr;
    other._ptr = null;
  }

  public unique_ptr(const unique_ptr<T>& other) = delete; // (conceptually)
}
```

- Copy constructors are selected for l-values; move constructors (r-value references) are preferred for r-values.
- Return value optimization (RVO) transfers ownership when returning locals or constructor temporaries by value.
- `operator=` must be provided explicitly for records with custom destructors.

## Smart pointers

The standard library provides C++-style smart pointers with Rust-style auto-deref:

```utp
import "utopia:memory";
using Memory;

class Widget {
  public int id;
  public void describe() { print("Widget %d\n", this.id); }
}

int main() {
  unique_ptr<Widget> u = make_unique<Widget>(new Widget());
  u.describe();     // auto-deref: (*u).describe()
  u.id = 42;        // auto-deref field write

  shared_ptr<Widget> s = make_shared<Widget>(new Widget());
  shared_ptr<Widget> copy = s;      // refcount 2
  print("%d\n", s.use_count());

  weak_ptr<Widget> w = s;           // non-owning
  if (!w.expired()) {
    shared_ptr<Widget> locked = w.lock();
  }
  return 0;
}
```

See [Smart Pointers](../stdlib/smart-pointers.md) for the full API.

## Rust-style auto-deref

When a member access (`obj.field` or `obj.method()`) does not resolve on the record itself, the compiler checks whether the object type overloads `operator*`. If so, it wraps the object in an implicit dereference and retries, up to 64 nested levels:

```utp
unique_ptr<shared_ptr<Widget>> nested = ...;
nested.describe();   // *(*nested).describe()
```

This is what makes smart pointers feel transparent: **there is no `->` operator in Utopia** — `.` always works.

## References

References alias existing objects and are dereferenced automatically on member access:

```utp
void bump(int& value) { value++; }

int main() {
  int x = 1;
  bump(x);
  print("%d\n", x);   // 2
  return 0;
}
```

R-value references (`T&&`) enable move semantics and are rejected when bound to l-values.
