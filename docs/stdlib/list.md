# List

`List<T>` is the prelude's generic dynamic array, backed by a growable heap buffer.

## Construction

```utp
List<int> numbers;                     // empty list (capacity 8)
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

- Backed by `T* _data`, `usize _length`, `usize _capacity`.
- The default constructor pre-allocates capacity for 8 elements.
- `push` grows the buffer by doubling.
- `ListLiteralView<T>` is a lightweight view (`const T* data; usize length;`) that enables `[1, 2, 3]` initialization without copying the literal twice.
- Like all records, `List` has a destructor and copy semantics; the compiler enforces proper copy/move construction.

## With smart pointers

`List` composes with generic smart pointers:

```utp
import "utopia:memory";
using std;

List<unique_ptr<Widget>> widgets;
widgets.push(make_unique<Widget>(new Widget()));
```
