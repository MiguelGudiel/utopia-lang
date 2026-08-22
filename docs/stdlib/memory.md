# Memory & Type Reflection

The prelude's `Memory/Core.utp` module exposes raw allocation and compile-time type reflection. The `Memory` namespace (`utopia:memory`) provides the manual allocation API.

## Raw allocation

```utp
@extern("malloc")
void* malloc(usize size);

@extern("free")
void free(void* ptr);
```

You normally use `new`/`delete` or `Memory.alloc` instead; these bindings exist for FFI and low-level work.

## Manual memory API: `Memory.alloc` / `construct` / `destruct` / `free`

```utp
import "utopia:memory";
using Memory;
```

### `Memory.alloc(size, align) -> RawMemory`

Allocates `size` bytes aligned to `align` (a power of two) without
constructing anything. Out-of-memory terminates the program (the same
policy as `new`).

```utp
RawMemory raw = Memory.alloc(sizeof(Item) * 2, alignof(Item));
```

### `Memory.construct<T>(ptr, args...) -> T*`

Constructs a `T` in existing memory and returns a `T*` aliasing `ptr`. The
destination pointer may be typed, `void*`, or a byte pointer. Constructor
arguments are forwarded; the storage is zeroed first so constructor
assignments see valid member state. `T` can be deduced from a typed
pointer:

```utp
Item* a = Memory.construct<Item>(raw.ptr, "x");
Item* b = Memory.construct(a + 1, "y");   // T deduced as Item
```

### `Memory.destruct(ptr)`

Runs the destructor of the pointee type, if it has one. Does not free the
memory.

### `Memory.free(raw)`

Releases a block previously returned by `Memory.alloc()`. Call it exactly
once per block.

### `RawMemory`

```utp
struct RawMemory {
  uint8* ptr;
}
```

### `Memory.isConst(ptr) -> bool`

Returns whether `ptr` points at a **canonical const object**: an instance
built at compile time in static read-only storage by a `const` expression
(see [Const & Canonicalization](../language/const.md)). Such objects are
immortal: their destructors never run and they must never be freed.
Frameworks that receive objects by pointer can query this before deleting:
if `isConst(ptr)` is true, skip the `delete`; otherwise free normally. The
check is O(N) over the program's const objects, which is negligible for
real-world counts.

```utp
Point* p = const Point(1, 2);     // canonical const object
print("%d\n", Memory.isConst(p)); // 1

Point* heap = new Point(9, 9);
print("%d\n", Memory.isConst(heap)); // 0
delete heap;                         // heap objects free normally
```

## Type reflection

Utopia provides compile-time `sizeof` and `typeof` intrinsics that produce **constants**.

### `sizeof`

```utp
usize bytes = sizeof(int32);       // 4
usize w = sizeof(Widget);          // record size
usize elem = sizeof(T);            // template parameter
```

### `alignof`

```utp
usize a = alignof(int32);          // 4
usize b = alignof(Widget);         // record alignment
usize c = alignof(T);              // template parameter
```

`alignof` honors the `@align(N)` annotation.

### `typeof`

`typeof` returns a reflection constant of type `Type`:

```utp
Type t = typeof(Widget);

if (t.isClass)    { /* ... */ }
if (t.isStruct)   { /* ... */ }
if (t.isEnum)     { /* ... */ }
if (t.isArray)    { /* ... */ }
if (t.isPointer)  { /* ... */ }
if (t.isPrimitive){ /* ... */ }
```

The `Type` struct:

```utp
struct Type {
  public uint8* name;
  public bool isClass;
  public bool isStruct;
  public bool isPrimitive;
  public bool isEnum;
  public bool isArray;
  public bool isPointer;
}
```

`MethodInfo` provides method reflection for records.

## Example

```utp
class Widget {
  public int id;
}

int main() {
  print("int32 size: %d\n", sizeof(int32));
  print("Widget size: %d\n", sizeof(Widget));

  Type t = typeof(Widget);
  print("isClass: %d\n", t.isClass);
  print("name: %s\n", t.name);
  return 0;
}
```
