# Map

Utopia ships three map types in the prelude, mirroring Dart's collection
library. All of them accept the same map literal syntax and deep-copy on
copy/assignment (like `List`):

| Type | Order | Lookup | Storage |
| --- | --- | --- | --- |
| `Map<K, V>` | insertion order (LinkedHashMap) | O(1) average | flat entries + chained buckets |
| `HashMap<K, V>` | unspecified (table order) | O(1) average | open addressing, linear probing |
| `SplayTreeMap<K, V>` | ascending key order | O(log n) amortized | splay tree |

## Construction

```utp
Map<String, int> scores = {"alice": 90, "bob": 85};   // map literal
HashMap<String, int> fast = {"a": 1, "b": 2};          // same literal
SplayTreeMap<String, int> sorted = {"b": 2, "a": 1};   // same literal
Map<String, int> empty = {};                            // empty literal
Map<String, int> copy = scores;                         // deep copy
```

- Keys of the literal may be string literals (`"alice"` becomes `String`),
  and values may be nested map or list literals
  (`Map<String, List<int>> m = {"a": [1, 2]};`).
- Duplicate keys in a literal keep the **last** value, like Dart.

## Methods

All three maps share this API:

| Member | Signature | Description |
| --- | --- | --- |
| `length()` | `usize length()` | Number of entries |
| `isEmpty()` / `isNotEmpty()` | `bool` | Empty check (O(1)) |
| `operator[]` | `V& operator[](K key)` | Indexes `key`, inserting a default-constructed value when absent (like `std::map`) |
| `put()` | `void put(K key, V value)` | Inserts or replaces the value (constructs once) |
| `containsKey()` | `bool containsKey(K key)` | Presence check (does not insert) |
| `containsValue()` | `bool containsValue(V value)` | Linear scan for a value |
| `remove()` | `bool remove(K key)` | Removes the entry; returns whether it existed |
| `clear()` | `void clear()` | Removes everything, keeping the storage |
| `forEach()` | `void forEach(void Function(K, V) fn)` | Runs `fn` for every entry |
| `keys()` | `List<K> keys()` | All keys (ordered per map type) |
| `values()` | `List<V> values()` | All values (ordered per map type) |

## Iteration order

```utp
int main() {
  Map<String, int> insertion = {"z": 1, "a": 2, "m": 3};
  List<String> keys = insertion.keys();
  // keys == ["z", "a", "m"], insertion order

  SplayTreeMap<String, int> sorted = {"z": 1, "a": 2, "m": 3};
  List<String> sortedKeys = sorted.keys();
  // sortedKeys == ["a", "m", "z"], ascending
  return 0;
}
```

## Requirements on key types

- `Map` and `HashMap` compare keys with `operator==` and hash them with the
  prelude's `hash<T>`; a key type must declare an overloaded `operator==`.
- `SplayTreeMap` orders keys with `operator<` (no `operator==` needed:
  equality is derived from the ordering). `String` and the numeric types
  already provide it.
- `hash<T>` hashes `String` by content, string literals by their
  null-terminated content, and every other type by its raw bytes, so equal
  keys always hash equally.

## Performance notes

- `Map` stores entries in one flat array (no per-entry allocation) plus a
  power-of-two bucket table; removals reuse slots through a free list.
  Insertions that grow the table move the entries, so, like C++'s
  `std::unordered_map`, references into the map are invalidated by
  inserting elements.
- `HashMap` probes a contiguous run of buckets and is the fastest of the
  three for lookups.
- `SplayTreeMap` splays every accessed node to the root, so recently
  accessed keys are the fastest afterwards; iteration walks the tree
  in-order with an explicit stack (never recursive).

## How it works

- The `{k: v, ...}` literal is typed by the compiler as an internal map
  literal type and lowered to **two parallel arrays** (keys and values)
  that back a `MapLiteralView<K, V>` (`const K* keys; const V* values;
  usize length;`), the map counterpart of `ListLiteralView<T>`.
- Any type with a constructor taking `MapLiteralView<K, V>` accepts the
  literal syntax, so user-defined map-like types work out of the box.
- Values are copy/move-constructed once into the map's storage
  (`Memory.construct`), and copies/assignments deep-copy every key and
  value, so two maps never share buffers.

## Example

```utp
int main() {
  Map<String, int> counters;
  counters.put("hits", 1);
  counters["hits"]++;
  counters["misses"] = 0;

  print("hits: %d\n", counters["hits"]);

  counters.forEach((key, value) {
    print("%s = %d\n", key.c_str(), value);
  });

  HashMap<int, String> byId = {1: "one", 2: "two"};
  SplayTreeMap<String, int> leaderboard = {"miguel": 9001, "ada": 42};
  List<String> rank = leaderboard.keys();   // ["ada", "miguel"]
  return 0;
}
```

## for-in

All three maps support Dart-style for-in loops through the structural
iterator protocol, with no `Iterable` inheritance and no vtables:

```utp
for (var k in counters) {            // iterating a map yields its KEYS
  print("%s = %d\n", k.c_str(), counters[k]);
}
for (var e in counters.entries()) {  // key/value pairs (MapEntry<K, V>)
  print("%s -> %d\n", e.key.c_str(), e.value);
}
for (var& k in leaderboard) {        // reference binding: no copies
  k = ...;
}
```

- `Map` iterates in insertion order, `SplayTreeMap` in ascending key order
  and `HashMap` in table order (unspecified).
- The cursors (`MapKeyIterator`, `MapEntryIterator`, ...) are tiny value
  types (map pointer + entry indices) whose `moveNext()`/`current()` are
  `@inline`: the optimized loop is a plain linked-list walk for `Map`, a
  bucket scan for `HashMap` and a parent-pointer tree walk for
  `SplayTreeMap`.
- Keys iteration yields `K&` (zero copies). `entries()` yields
  `MapEntry<K, V>` by value, so each pair is copied; use `map[k]` for
  zero-copy value access while iterating keys.
- `map.entries()` returns a non-owning view (a pointer to the map, no
  copy). Mutating a map while iterating invalidates the cursor.
```
