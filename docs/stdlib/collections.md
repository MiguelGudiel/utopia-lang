# Collections

The stdlib `collections.utp` module (imported with `import "utopia:collections";`)
provides the specialized map types and the functional data structures.
The fundamental `List<T>` and `Map<K, V>` stay in the prelude.

| Type | Order | Complexity | Storage |
| --- | --- | --- | --- |
| `HashMap<K, V>` | unspecified (table order) | O(1) average | open addressing, linear probing |
| `SplayTreeMap<K, V>` | ascending key order | O(log n) amortized | splay tree |
| `Set<T>` | unspecified | O(1) average | hash table (over `HashMap`) |
| `Stack<T>` | LIFO | O(1) | `List` tail |
| `Queue<T>` | FIFO | O(1) amortized | circular buffer |
| `Deque<T>` | double-ended | O(1) amortized | circular buffer |
| `PriorityQueue<T>` | min-heap (smallest first) | O(log n) push/pop | binary heap over `List` |

## Stack

```utp
Stack<int> s;
s.push(1);
s.push(2);
int top = s.top();      // 2, without removing
int v = s.pop();        // 2
```

## Queue / Deque

`Queue` is a FIFO queue: `push` appends to the back, `pop` removes from
the front. `Deque` adds `pushFront`/`popFront`/`popBack`, `first`, `last`,
indexing, and supports `for-in`:

```utp
Queue<String> q;
q.push("a");
String next = q.pop();

Deque<int> d;
d.pushBack(1);
d.pushFront(0);
for (int x in d) { ... }   // 0, 1
```

Both grow by doubling the circular buffer; elements are moved once per
growth, like `List`.

## PriorityQueue

A min-heap: `first()` is the smallest element (by `operator<` on `T`,
which `String` and numbers provide), `pop()` removes it in O(log n):

```utp
PriorityQueue<int> pq;
pq.push(5); pq.push(1); pq.push(3);
while (pq.isNotEmpty()) {
  print("%d ", pq.pop());   // 1 3 5
}
```

## Set

A hash set backed by `HashMap`: elements must provide `operator==` (like
`HashMap` keys). Duplicates are ignored; iteration order is unspecified:

```utp
Set<String> seen;
seen.add("player-1");
seen.add("player-1");       // duplicate, ignored
bool present = seen.contains("player-1");
List<String> all = seen.toList();
```
