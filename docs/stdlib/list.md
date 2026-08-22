# List

`List<T>` is the prelude's generic dynamic array with `std::vector`-style
semantics, backed by a raw aligned heap buffer.

## Construction

```utp
List<int> numbers;                     // empty list (no allocation yet)
List<int> fromLiteral = [1, 2, 3];     // from an array literal (via ListLiteralView<T>)
List<int> copy = numbers;              // copy constructor
```

## Methods

| Member | Signature | Description |
| --- | --- | --- |
| `length()` | `usize length()` | Number of elements |
| `capacity()` | `usize capacity()` | Current capacity |
| `push()` | `void push(T item)` | Append an element (doubles capacity when full) |
| `operator[]` | `T& operator[](usize index)` | Index access (read/write) |

## Example

```utp
import "utopia:memory";

int main() {
  List<int> scores = [90, 85, 100];

  scores.push(95);
  scores[0] = 99;

  print("length: %d\n", scores.length());
  for (usize i = 0; i < scores.length(); i++) {
    print("%d ", scores[i]);
  }
  print("\n");
  return 0;
}
```

## How it works

- Storage is a `RawMemory` block allocated with `Memory.alloc(size, align)`;
  **elements are constructed lazily** — an empty `List` performs zero
  allocations and no element is ever default-constructed unless it is
  actually stored.
- `push`, `insert` and `removeAt` construct/destruct only the affected
  elements: new slots are constructed from the argument, removed elements
  are destructed, and shifts are moves.
- Reallocation (`push` when full) moves the live elements into a new
  `Memory.alloc` block with `Memory.construct` + `Memory.destruct`, so
  types with move constructors (e.g. `String`) are **moved — no copy and
  no per-element allocation**. Without a move constructor the copy
  constructor is used; trivially copyable records are bitwise-copied.
- `clear()` destructs the live elements and keeps the capacity.
- `ListLiteralView<T>` is a lightweight view (`const T* data; usize length;`) that enables `[1, 2, 3]` initialization without copying the literal twice.
- Like all records, `List` has a destructor and copy semantics; the compiler enforces proper copy/move construction.

## for-in

`List` (and `ListLiteralView`) support Dart-style for-in loops through the
structural iterator protocol — no `Iterable` inheritance, no vtables:

```utp
for (var v in scores) {      // copies each element (Dart semantics)
  print("%d ", v);
}
for (var& v in scores) {     // reference binding: no copy, writes through
  v = v * 2;
}
for (var x in [1, 2, 3]) {   // array literals iterate directly
  print("%d ", x);
}
```

`ListIterator<T>` is a value type holding the element base pointer, an
offset and the length; `moveNext()`/`current()` are `@inline`, so the
optimized loop is a raw pointer walk identical to a manual index loop.
Mutating the list while iterating invalidates the cursor (reallocations
move the elements).

## With smart pointers

`List` composes with generic smart pointers:

```utp
import "utopia:memory";
using Memory;

List<unique_ptr<Widget>> widgets;
widgets.push(make_unique<Widget>(new Widget()));
```
