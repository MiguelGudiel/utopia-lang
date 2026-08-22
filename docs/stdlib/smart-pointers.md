# Smart Pointers

Utopia's standard library provides C++-style smart pointers with **Rust-style auto-deref**: member access with `.` resolves through `operator*` automatically, so there is no `->` syntax.

```utp
import "utopia:memory";
using Memory;
```

## `unique_ptr<T>`

Owns a heap object exclusively. Cannot be copied; ownership is transferred with move semantics.

| Member | Description |
| --- | --- |
| `unique_ptr()` | Empty pointer |
| `unique_ptr(T* p)` | Adopt a raw pointer |
| `unique_ptr(unique_ptr<T>&& other)` | Move constructor (steals ownership) |
| `~unique_ptr()` | Deletes the owned object |
| `operator=(unique_ptr<T>&& other)` | Move assignment |
| `operator*() → T*` | Raw pointer access (drives auto-deref) |
| `get() → T*` | Raw pointer |
| `release() → T*` | Transfer ownership out (pointer becomes empty) |
| `reset()` | Delete owned object, become empty |
| `reset(T* p)` | Replace owned object |
| `operator==(T* other)` / `operator!=(T* other)` | Compare with raw pointer / `null` |
| `operator==(const unique_ptr<T>&)` / `!=` | Compare two unique_ptrs |

```utp
unique_ptr<Widget> u = make_unique<Widget>(new Widget());
u.describe();                 // auto-deref
u.id = 42;                    // auto-deref field write

Widget* raw = u.release();    // hand off ownership
delete raw;

u.reset(new Widget());
if (u == null) { /* unreachable */ }
```

## `shared_ptr<T>`

Reference-counted shared ownership. Multiple shared_ptrs own the same object; the object is destroyed when the last owner releases it. The control block is shared with `weak_ptr`.

| Member | Description |
| --- | --- |
| `shared_ptr()` | Empty |
| `shared_ptr(T* p)` | Adopt with a fresh control block |
| `shared_ptr(const shared_ptr<T>& other)` | Copy (bumps count) |
| `shared_ptr(shared_ptr<T>&& other)` | Move |
| `~shared_ptr()` | Release one reference |
| `operator=(const shared_ptr<T>&)` / `operator=(shared_ptr<T>&&)` | Copy/move assignment |
| `operator*() → T*` | Raw access (auto-deref) |
| `get() → T*` | Raw pointer |
| `use_count() → int` | Current reference count |
| `reset()` / `reset(T* p)` | Release / replace |
| `operator==/!=(T*)` and `operator==/!=(const shared_ptr<T>&)` | Comparisons |

```utp
shared_ptr<Widget> s = make_shared<Widget>(new Widget());
{
  shared_ptr<Widget> copy = s;          // use_count: 2
  print("%d\n", s.use_count());
}                                       // use_count: 1
```

## `weak_ptr<T>`

A non-owning observer of a `shared_ptr`. It does not keep the object alive and never causes leaks.

| Member | Description |
| --- | --- |
| `weak_ptr()` | Empty |
| `weak_ptr(const shared_ptr<T>& sp)` | Observe a shared_ptr |
| `expired() → bool` | Whether the owned object is still alive |
| `lock() → shared_ptr<T>` | Upgrade to a shared_ptr (empty if expired) |

```utp
weak_ptr<Widget> w = s;
if (!w.expired()) {
  shared_ptr<Widget> locked = w.lock();
  locked.describe();
}
s.reset();
print("%d\n", w.expired());   // 1, the object is gone
```

## Factories

```utp
unique_ptr<T> make_unique<T>(T* p);
shared_ptr<T> make_shared<T>(T* p);
```

They adopt an already-constructed heap object, mirroring `Memory.make_unique`/`Memory.make_shared` ownership semantics (and C++'s `std::make_unique`/`std::make_shared`). Factories can also return by value with RVO:

```utp
unique_ptr<Widget> makeWidget() {
  return unique_ptr<Widget>(new Widget());
}
```

## Auto-deref details

Member access resolution works like this:

1. Look up the member on the smart pointer itself (its own methods win: `get`, `reset`, ...).
2. If not found, check whether the type overloads `operator*`; if so, dereference implicitly and retry.
3. Repeat up to 64 levels, so nested smart pointers work: `unique_ptr<shared_ptr<T>>`.

This mirrors Rust's auto-deref and is why `ptr.field`, `ptr.method()`, and even `ptr.field = value` work transparently.

## Memory model notes

- Smart pointers are value types with destructors; the compiler enforces copy/move correctness (no implicit copies of destructor-bearing records).
- `shared_ptr` allocates the control block separately (single-owner objects like `unique_ptr` are allocation-free beyond the owned object).
- The implementation is plain Utopia in `libs/stdlib/lib/memory.utp`; read it to see the full reference-counting logic.
