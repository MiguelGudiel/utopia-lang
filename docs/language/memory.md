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

Records with custom destructors cannot be implicitly copied; a copy constructor or move constructor must be provided, preventing accidental double frees at compile time.

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

## Raw memory: `Memory.alloc` / `Memory.free`

For untyped, aligned storage the standard library provides a Zig/Odin-style
memory API (`import "utopia:memory"; using Memory;`):

```utp
RawMemory raw = Memory.alloc(sizeof(Buffer) * 2, alignof(Buffer));

Buffer* b0 = Memory.construct<Buffer>(raw.ptr, "x", 1);
Buffer* b1 = Memory.construct<Buffer>(raw.ptr + sizeof(Buffer), "y", 2);

Memory.destruct(b0);        /* runs ~Buffer, does not free */
Memory.destruct(b1);
Memory.free(raw);           /* releases the whole block */
```

- `Memory.alloc(size, align)` returns a `RawMemory { uint8* ptr }` block
  aligned to `align` (a power of two); no constructor runs.
  Out-of-memory terminates the program (the same policy as `new`).
- `Memory.construct<T>(ptr, args...)` constructs an object of type `T` in
  existing memory (typed pointers, `void*`, or byte pointers such as
  `RawMemory.ptr` all work) and returns a `T*` aliasing `ptr`. The
  destination is zeroed first so constructor assignments
  (`this.field = value`) always see a valid object state. `T` may be
  deduced from the pointer when it is already typed:
  `Memory.construct(b0, "x")`.
- `Memory.destruct(ptr)` runs the destructor of the pointee type (if it has
  one); it never frees the memory.
- `Memory.free(raw)` releases the block. `RawMemory` is an owning handle:
  release each block exactly once.
- Pointer arithmetic is element-scaled: `raw.ptr + sizeof(Buffer)` steps by
  bytes because `RawMemory.ptr` is a byte pointer, while `b0 + 1` steps by
  one `Buffer`.

This replaces the old C++-style `new uninitialized T` / `delete uninitialized`
and placement-new (`new (ptr) T(...)`) forms, which have been removed.

## Custom allocators: `operator new` / `operator delete`

`new` / `delete` route through user-defined allocators, exactly like C++.
Declare them as static class methods (they take precedence for that type) or
as file-scope functions (they apply to every `new` in that module):

```utp
/* Module-wide custom allocator */
void* operator new(usize size) {
  return myPoolAllocate(size);
}
void operator delete(void* ptr) {
  myPoolFree(ptr);
}

class Tracked {
  /* Class-level allocator: only for 'new Tracked' */
  public static void* operator new(usize size) { ... }
  public static void operator delete(void* ptr) { ... }
}
```

The required signatures are `void* operator new(usize size)` and
`void operator delete(void* ptr)`. Arrays (`new T[n]` / `delete[]`) also
route through the custom allocators.

## Out-of-memory

Utopia has no exceptions (there is no try/catch), so a failed allocation
cannot be thrown from. When the allocator returns null, the program
terminates (the analogue of C++ `std::bad_alloc`). A custom `operator new`
can return null to trigger this, or handle the failure itself.

## Alignment: `@align`

The `@align(N)` annotation sets the alignment of structs, classes, unions
and variables; `alignof(T)` reports it at compile time:

```utp
@align(16)
class Vec4 {
  public float32 x;
  public float32 y;
  public float32 z;
  public float32 w;
}

usize a = alignof(Vec4);   // 16
```

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

This is what makes smart pointers feel transparent: **there is no `->` operator in Utopia**, so `.` always works.

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
