# Async / Await

Utopia supports Dart-style asynchronous programming: `async` functions, methods,
lambdas and even `main` compile to LLVM coroutines that return a `Future<T>` of
their declared return type, and `await` suspends the coroutine until the awaited
future completes.

## The event loop

There is one event loop per thread. A coroutine's continuations always resume
on the loop of the thread that registered them (strict thread affinity, the
Dart isolate model): completing a future from any thread posts the
continuations to their owner loops, and only the owner thread drains its own
loop. There is no background scheduler, no thread-stealing and no timer
thread: the runtime is driven exclusively by its owners, so secondary threads
can never mutate async state that belongs to the render thread.

- Plain programs (an `async main`) are driven by the runtime's blocking
  drivers, which sleep exactly until the next timer deadline (or until a
  continuation lands) and then drain the calling loop.
- Host-driven programs (a game/UI engine, an embedded loop) invert the
  control: the host sleeps until `min(vsync, next deadline)` and calls
  `__loop_drain__()` once per frame. The call never blocks — it expires due
  timers, swaps the shared queue into a local deque in O(1) under the lock,
  runs the pending microtasks without holding it, and returns as soon as the
  queue is quiescent. A chain of `await` completions settles within a single
  call (Dart microtask semantics).

The host-facing API is exported without name mangling, so it can be called
from Utopia code (via `@extern`), from a Utopia library, and from plain C/C++:

```utp
@extern("__loop_drain__")
public void __loop_drain__();

/* Optional: time-boxed drain and next-deadline query for engine loops. */
@extern("__loop_drain_budget__")
public int __loop_drain_budget__(int64 maxUs);

@extern("__loop_next_deadline_us__")
public int64 __loop_next_deadline_us__();

/* Optional: frame-capping sleep for engine loops. */
@extern("__loop_sleep_until_deadline__")
public int __loop_sleep_until_deadline__(int64 maxSleepMs);
```

```cpp
extern "C" {
void __loop_drain__();
int __loop_drain_budget__(int64_t maxUs);
int64_t __loop_next_deadline_us__();
int __loop_sleep_until_deadline__(int64_t maxSleepMs);
}

/* Engine loop: sleep until the next deadline (vsync or timer), then pump
 * the microtask queue within the frame budget. */
for (;;) {
  int64_t due = __loop_next_deadline_us__();
  int64_t sleepUs = vsyncUs;
  if (due > 0 && due - nowUs() < sleepUs)
    sleepUs = due - nowUs();
  sleep(sleepUs);

  /* __loop_drain_budget__ never blocks and stops once the budget elapses;
   * it returns 1 while work is still pending. A non-positive budget is a
   * pure poll. __loop_drain__() drains without a budget. */
  if (__loop_drain_budget__(frameBudgetUs))
    continue; /* or render anyway and catch up next frame */
  render();
}
```

`__loop_sleep_until_deadline__(maxSleepMs)` sleeps on the calling thread
until the loop's next timer deadline or until `maxSleepMs` ms elapse,
whichever comes first (returns 1 when the deadline was the reason, 0 on
timeout). It evaluates the deadline on the runtime's own steady clock, so a
host frame capper never has to reconcile two clocks: sleep until
`min(vsync, deadline)` and a `Future.delayed` timer settles in the next
frame's drain exactly on time instead of up to a frame late.

Timers (`Future.delayed`) are deadline entries owned by the loop that created
them; they are completed by the loop's owner when it drains, so no helper
thread is needed and a timer can never fire "between" microtasks of a running
drain. See `utopia-sandboxes/async-ui` for a full Flutter-like test suite (a
framework owning the frame loop, static and dynamic linking, C++ hosts, and
frame-budget protection against microtask storms).

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

## Error handling

Async functions support `try`/`catch`/`throw` like synchronous ones. An
exception that no catch clause inside the coroutine handles is captured into
the function's future (Dart semantics): awaiting that future rethrows the
error at the await site, with its dynamic type preserved, so a surrounding
`try`/`catch` in the awaiting coroutine can catch it:

```utp
class NetworkError {
  int32 code;
  NetworkError(this.code) {}
}

Future<int> fetch() async {
  await Future.delayed<void>(10, () {});
  throw NetworkError(404);
}

int main() async {
  try {
    int v = await fetch();
    print("value %d\n", v);
  } catch (NetworkError e) {
    print("failed with %d\n", e.code);
  }
  return 0;
}
```

Awaiting a failed future rethrows even outside a `try`; if nothing handles
the error, the future carries it until it is released, and the runtime then
reports it (`Unhandled async error: ...`) and terminates the process.

Callback-style error handling is available on `Future<T>`:

```utp
/* catchError: the handler runs on error and its result completes the
 * returned future. A successful source passes its value through. */
int v = await fetch().catchError(() => -1);

/* The handler may be async. */
int v = await fetch().catchError(() async {
  await Future.delayed<void>(5, () {});
  return -1;
});

/* then with an onError handler: it runs instead of the value callback. */
await fetch().then((v) => print("value %d\n", v),
                   () => print("failed\n"));

/* timeout: the deadline wins and completes with a TimeoutException, or
 * with the onTimeout callback's result when one is given. */
try {
  await slowFetch().timeout(Duration(seconds: 1));
} catch (TimeoutException e) {
  print("timed out\n");
}
int v = await slowFetch().timeout(Duration(seconds: 1), () => -1);
```

Errors flow through `then` chains: a failed source completes the chain's
result future with the same error, so one `catchError` at the end of a chain
(or one `try` around the final `await`) handles errors from any link.

## The standard library

- `Future.value<T>(v)`: a future already completed with `v`
- `Future.sync<T>(fn)`: runs `fn` immediately, completes with its result
- `Future.delayed<T>(ms, fn)`: completes with `fn()`'s result after `ms` ms
- `Future.runOnThread<T>(fn)`: runs `fn` on a worker thread
- `Future.wait<T>(List<Future<T>>)`: completes with the values, in order
- `f.then(cb)`: runs `cb` when the future completes (sync or async callbacks)
- `f.then(cb, onError)`: runs `onError` instead of `cb` when the future failed
- `f.catchError(onError)`: recovers from an error with the handler's result
- `f.timeout(limit, [onTimeout])`: fails with `TimeoutException` when the
  deadline passes first, or completes with `onTimeout()`'s result
- `f.whenComplete(cb)`: runs `cb` when the future completes
- `f.isCompleted()`: true once the future has settled

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
