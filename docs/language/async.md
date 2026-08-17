# Async / Await

Utopia supports Dart-style asynchronous programming: `async` functions, methods,
lambdas and even `main` compile to LLVM coroutines that return a `Future<T>` of
their declared return type, and `await` suspends the coroutine until the awaited
future completes.

## Async functions

Mark a function with `async`. Its declared return type becomes the *value* type;
the function actually returns a `Future<T>`:

```utp
Future<int> fetchData() async {
  await Future.delayed<void>(100, () {});
  return 42;
}
```

`await` suspends the current coroutine until the future completes and yields its
value. Awaiting a non-`Future` expression passes the value through.

## Async main

`main` can be `async` too. The driver runs the event loop until main's future
completes, then exits with status 0:

```utp
int main() async {
  int value = await fetchData();
  print("value: %d\n", value);
  return 0;
}
```

## Fire-and-forget

Calling an async function without `await` starts it in the background, exactly
like Dart. The event loop keeps the program alive until every pending future
(timers, worker threads, continuations) settles:

```utp
void backgroundUpdate() async {
  await Future.delayed<void>(300, () {});
  globalState = 7;
}

int main() async {
  backgroundUpdate();          // runs in the background
  int a = await fetchData();   // meanwhile main keeps going
  // by the time main finishes, backgroundUpdate has settled
  return 0;
}
```

A synchronous `main` drains the event loop after returning, so fire-and-forget
work started there still completes before the process exits.

## Async lambdas

Lambdas can be `async` as well; their type is `Future<R> Function(...)`:

```utp
Future<int> Function() lazy = () async {
  await Future.delayed<void>(20, () {});
  return 99;
};

int v = await lazy();
```

`Future.sync` and `Future.delayed` accept synchronous callbacks; `then()` has
overloads for both sync and async callbacks. An async lambda cannot be passed
where a synchronous function is expected (and vice versa).

## Future<T> annotations

An `await` may be assigned into an explicitly-typed `Future<T>` variable; the
await then passes the future through instead of unwrapping:

```utp
Future<int> fetchTwice() async {
  Future<int> first = await fetchData();  // first keeps the future
  return first;                           // returning it awaits implicitly
}
```

## Threads

`Future.runOnThread` runs a function on a real worker thread and completes the
returned future with its result:

```utp
int t = await Future.runOnThread<int>(() => 100 + 50);
```

An `async` lambda passed to `runOnThread` runs on the worker with its own copy
of the runtime, so `await` keeps working there:

```utp
int u = await Future.runOnThread<int>(() async {
  int inner = await fetchData();
  return inner * 2;
});
```

## The standard library

- `Future.value<T>(v)` — a future already completed with `v`
- `Future.sync<T>(fn)` — runs `fn` immediately, completes with its result
- `Future.delayed<T>(ms, fn)` — completes with `fn()`'s result after `ms` ms
- `Future.runOnThread<T>(fn)` — runs `fn` on a worker thread
- `Future.wait<T>(List<Future<T>>)` — completes with the values, in order
- `f.then(cb)` — runs `cb` when the future completes (sync or async callbacks)
- `f.whenComplete(cb)` — runs `cb` when the future completes
- `f.isCompleted()` — true once the future has settled

## Enabling and disabling the runtime

Async support is on by default. The `Future` template is part of the prelude
(guarded by the `UTOPIA_ASYNC` macro) and the runtime (`libutopia_async`) is
linked automatically.

To disable it:

- `utopia build --no-async` / `utopia run --no-async`, or
- `async: false` in `build.yaml`:

```yaml
build:
  target: executable
  sources:
    - "main.utp"
  async: false
```

With async disabled, `async`/`await` produce clear compile-time errors and no
runtime is linked.
