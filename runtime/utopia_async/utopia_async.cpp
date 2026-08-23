/*
 * Utopia Async Runtime
 *
 * A small, self-contained runtime that powers Utopia's Dart-style
 * async/await support on top of LLVM coroutines.
 *
 * Model
 * -----
 * - Every async function compiles to an LLVM coroutine. Its "ramp" creates
 *   a future state (via utopia_future_create), runs the body eagerly up to
 *   the first await, and returns a Future object.
 * - `await f` lowers to: utopia_future_is_completed(f) -> either read the
 *   value inline, or register the coroutine's resume function with
 *   utopia_future_then and suspend (llvm.coro.suspend).
 * - Futures are reference counted. A Future object holds one reference;
 *   every registered continuation holds one reference until it runs.
 * - There is one event loop per thread (thread_local). Continuations are
 *   always resumed on the loop that registered them, which makes the
 *   runtime safe to use from multiple threads: completing a future from
 *   any thread posts the continuations to their owner loops.
 *
 * Thread affinity
 * ---------------
 * - A loop's queue is only ever drained by its owning thread. There is no
 *   background scheduler and no thread-stealing: work that arrives at a
 *   loop sits in its queue until the owner drains it. This is the Dart
 *   isolate model -- strict single-threadedness per loop -- which makes
 *   the runtime safe for hosts that render continuously (a UI engine): no
 *   secondary thread can mutate async state that belongs to the render
 *   thread.
 * - Host-driven programs (a game/UI engine, an embedded loop) call
 *   __loop_drain__() once per frame: it swaps the shared queue into a
 *   local deque in O(1) under the lock and runs the continuations without
 *   holding it. The call never blocks and returns as soon as no microtask
 *   is pending, so the host keeps full control of the frame budget.
 * - The blocking drivers (utopia_loop_run / utopia_loop_run_all /
 *   utopia_future_wait) are built on top of the same drain primitive; they
 *   only exist for plain executables whose process entry is async main.
 *   They sleep on the loop's condition variable (woken by every post) and
 *   never touch another loop's queue.
 *
 * Threads
 * -------
 * - Future.runOnThread spawns a worker thread (utopia_thread_spawn). The
 *   worker runs a compiler-generated thunk that calls the user function
 *   and completes the future state; the completion is posted back to the
 *   owner loop, so `await` on it blocks the loop until the thread
 *   finishes. An async lambda passed to runOnThread runs on the worker
 *   with its own event loop copy (its own drain).
 * - Timers (Future.delayed) are deadline entries in the loop that created
 *   them. There is no global timer thread: the loop's owner expires the
 *   timers when it drains, and the blocking drivers (and the host, via
 *   __loop_next_deadline_us__) sleep exactly until the next deadline, so
 *   the runtime never spawns helper threads for timers.
 *
 * Memory
 * ------
 * - All states and continuation blocks are allocated with plain malloc and
 *   freed when their reference count drops to zero. No GC.
 *
 * The layout of the future state is private to this file: the compiler
 * only interacts with states through the functions below, so the two sides
 * can evolve independently.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct UtopiaContinuation {
  UtopiaContinuation *next;
  struct UtopiaEventLoop *loop;
  void (*resumeFn)(void *frame);
  void *frame;
};

struct UtopiaFutureState;

struct UtopiaTimer {
  int64_t dueUs;
  UtopiaFutureState *state;
  UtopiaTimer *next;
};

struct UtopiaEventLoop {
  std::mutex mtx; /* guards queue + timers */
  /* Notified on every post to this loop and on every timer insertion; the
   * owner's blocking drivers wait on it (with waitMtx as the wait lock)
   * and wake up immediately when a continuation lands or a deadline
   * changes. */
  std::condition_variable cv;
  /* Dedicated lock for driver waits: drivers sleep on cv without holding
   * mtx, so posters are never delayed by an idle driver. */
  std::mutex waitMtx;
  std::deque<UtopiaContinuation *> queue;
  /* Deadline-sorted timer list (earliest first). Expired by the owner
   * when it drains; there is no global timer thread. */
  UtopiaTimer *timers = nullptr;
  std::atomic<int32_t> activeThreads{0};
  std::atomic<int32_t> pendingTimers{0};
  /* Global registry link (guarded by gLoopRegistryMtx). */
  UtopiaEventLoop *next = nullptr;
};

struct UtopiaFutureState {
  std::atomic<int32_t> refs;
  std::atomic<int32_t> flags; /* bit 0: completed */
  /* Recursive: completing a future posts its continuations, whose resumption
   * can complete another future (or the same one through a resumed coroutine
   * chain) on the same thread while the outer completion still holds the
   * lock. */
  std::recursive_mutex mtx; /* guards head + flags */
  UtopiaContinuation *head;
  UtopiaEventLoop *ownerLoop;
  uint64_t valueSize;
  uint32_t valueAlign;
  void (*valueDtor)(void *valuePtr);
  /* The value storage follows the struct, aligned to valueAlign. */
};

enum : uint32_t { kUtopiaFutureCompleted = 1u };

static thread_local UtopiaEventLoop *tlsLoop = nullptr;

extern "C" void utopia_future_complete(void *state);

/* ------------------------------------------------------------------ */
/* Loop registry                                                       */
/*                                                                     */
/* Kept so the blocking drivers can tell whether any loop still has    */
/* work that could complete a future (queued continuations, worker     */
/* threads, scheduled timers). It is only ever *read* here: no thread  */
/* pumps another thread's queue.                                       */
/* ------------------------------------------------------------------ */

static std::mutex gLoopRegistryMtx;
static UtopiaEventLoop *gLoops = nullptr;

static void registerLoop(UtopiaEventLoop *loop) {
  std::lock_guard<std::mutex> lock(gLoopRegistryMtx);
  loop->next = gLoops;
  gLoops = loop;
}

static UtopiaEventLoop *getLoop() {
  if (!tlsLoop) {
    tlsLoop = new UtopiaEventLoop();
    registerLoop(tlsLoop);
  }
  return tlsLoop;
}

static int64_t nowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
      .count();
}

static void *futureValuePtr(UtopiaFutureState *s) {
  uint64_t base = sizeof(UtopiaFutureState);
  uint64_t align = s->valueAlign ? s->valueAlign : 1;
  uint64_t off = (base + align - 1) & ~(align - 1);
  return reinterpret_cast<char *>(s) + off;
}

static void releaseState(UtopiaFutureState *s) {
  if (s->refs.fetch_sub(1) == 1) {
    if (s->valueSize && s->valueDtor)
      s->valueDtor(futureValuePtr(s));
    delete s;
  }
}

static void postContinuation(UtopiaContinuation *c) {
  UtopiaEventLoop *loop = c->loop ? c->loop : getLoop();
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    loop->queue.push_back(c);
  }
  loop->cv.notify_one();
}

/* True when any loop still has work that could complete futures (queued
 * continuations, worker threads, or scheduled timers). */
static bool hasAnyPendingWork() {
  UtopiaEventLoop *it;
  {
    std::lock_guard<std::mutex> lock(gLoopRegistryMtx);
    it = gLoops;
  }
  while (it) {
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(it->mtx);
      busy = !it->queue.empty();
    }
    if (busy || it->activeThreads.load() > 0 || it->pendingTimers.load() > 0)
      return true;
    it = it->next;
  }
  return false;
}

/* True when the loop's earliest timer is due at 'now'. Caller holds mtx. */
static bool loopHasDueTimer(UtopiaEventLoop *loop, int64_t now) {
  return loop->timers && loop->timers->dueUs <= now;
}

/* Completes the state of every timer that is due at 'now'. Safe to call
 * from the loop's owner only (timers belong to the loop that created
 * them). The states are completed outside the loop lock: completing can
 * post continuations to other loops. */
static void expireTimers(UtopiaEventLoop *loop, int64_t now) {
  std::vector<UtopiaFutureState *> due;
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    while (loop->timers && loop->timers->dueUs <= now) {
      UtopiaTimer *t = loop->timers;
      loop->timers = t->next;
      due.push_back(t->state);
      loop->pendingTimers.fetch_sub(1);
      delete t;
    }
  }
  for (UtopiaFutureState *s : due)
    utopia_future_complete(s);
}

/* Earliest pending deadline of 'loop' in microseconds, or -1 when no
 * timer is scheduled. Caller holds mtx. */
static int64_t loopDeadlineUs(UtopiaEventLoop *loop) {
  return loop->timers ? loop->timers->dueUs : -1;
}

/* Sleeps until a post lands on 'loop' or until the loop's next timer
 * deadline, whichever comes first (a missed-wakeup timeout covers races).
 * The owner driver never holds loop->mtx here, so producers are never
 * blocked by an idle driver. */
static void driverIdle(UtopiaEventLoop *loop) {
  int64_t due = 0;
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    due = loopDeadlineUs(loop);
  }
  std::unique_lock<std::mutex> lock(loop->waitMtx);
  if (due > 0) {
    int64_t now = nowUs();
    if (due <= now)
      return;
    loop->cv.wait_for(lock, std::chrono::microseconds(due - now));
  } else {
    loop->cv.wait_for(lock, std::chrono::milliseconds(5));
  }
}

extern "C" {

/* ------------------------------------------------------------------ */
/* Event loop drain (inversion of control)                             */
/* ------------------------------------------------------------------ */

/* Drains the calling thread's event loop: expires due timers and swaps
 * the shared queue into a local deque under the lock (O(1), lock held for
 * an instant), running the continuations without holding it. Tasks posted
 * while draining are picked up by the next swap, so microtasks behave
 * like Dart's: a chain of await completions settles within a single call.
 * Never blocks.
 *
 * When 'budgetUs' > 0 the drain stops once that many microseconds elapsed
 * (measured between batch boundaries), so a host can protect its frame
 * budget from microtask storms. Returns 1 when work is still pending
 * (microtasks or due timers), 0 when the loop is quiescent.
 *
 * This is the host-facing primitive: a UI/game engine calls it once per
 * frame instead of letting the runtime block the main thread. It is
 * exported without name mangling so it can be reached from Utopia (via
 * @extern), from a Utopia library, and from plain C/C++ hosts. */
static int drainLoop(UtopiaEventLoop *loop, int64_t budgetUs) {
  int64_t start = budgetUs > 0 ? nowUs() : 0;
  for (;;) {
    expireTimers(loop, nowUs());
    std::deque<UtopiaContinuation *> local;
    {
      std::lock_guard<std::mutex> lock(loop->mtx);
      if (loop->queue.empty() && !loopHasDueTimer(loop, nowUs()))
        return 0;
      local.swap(loop->queue);
    }
    for (UtopiaContinuation *c : local) {
      c->resumeFn(c->frame);
      delete c;
    }
    if (budgetUs > 0 && nowUs() - start >= budgetUs) {
      std::lock_guard<std::mutex> lock(loop->mtx);
      if (loop->queue.empty() && !loopHasDueTimer(loop, nowUs()))
        return 0;
      return 1;
    }
  }
}

void __loop_drain__() { drainLoop(getLoop(), 0); }

/* Time-boxed drain: processes at most 'maxUs' of microtask work and
 * returns 1 when the loop still has pending work (microtasks or due
 * timers), 0 when it is quiescent. A non-positive budget performs a pure
 * poll: no microtask is processed, the answer reflects the state at that
 * instant. */
int __loop_drain_budget__(int64_t maxUs) {
  UtopiaEventLoop *loop = getLoop();
  if (maxUs <= 0) {
    std::lock_guard<std::mutex> lock(loop->mtx);
    return (!loop->queue.empty() || loopHasDueTimer(loop, nowUs())) ? 1 : 0;
  }
  return drainLoop(loop, maxUs);
}

/* Earliest pending timer deadline of the calling thread's loop, in
 * microseconds (steady clock), or -1 when no timer is scheduled. A host
 * engine sleeps until min(vsync, deadline) and then drains, exactly like
 * a native event loop sleeping on epoll with a timeout. */
int64_t __loop_next_deadline_us__() {
  UtopiaEventLoop *loop = getLoop();
  std::lock_guard<std::mutex> lock(loop->mtx);
  return loopDeadlineUs(loop);
}

/* Frame-capping sleep for host engines: sleeps on the calling thread until
 * the loop's next timer deadline or until 'maxSleepMs' milliseconds elapse,
 * whichever comes first. Returns 1 when a timer deadline was the reason (it
 * was already due or was reached during the sleep), 0 when the timeout
 * elapsed first. The deadline is evaluated on the runtime's own steady
 * clock, so a host never has to reconcile two clocks. Sleeping until the
 * deadline lets a 'Future.delayed' timer settle in the next frame's drain
 * exactly on time instead of up to a frame late. */
int __loop_sleep_until_deadline__(int64_t maxSleepMs) {
  UtopiaEventLoop *loop = getLoop();
  int64_t due = 0;
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    due = loopDeadlineUs(loop);
  }
  if (due < 0)
    return 0;
  int64_t now = nowUs();
  if (due <= now)
    return 1;
  int64_t maxUs = maxSleepMs > 0 ? maxSleepMs * 1000 : 0;
  int64_t sleepUs = due - now;
  if (maxUs > 0 && sleepUs > maxUs)
    sleepUs = maxUs;
  std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
  return nowUs() >= due ? 1 : 0;
}

/* Compatibility alias for the previous drain API name. */
void utopia_loop_drain() { __loop_drain__(); }

/* ------------------------------------------------------------------ */
/* Future state creation / refcounting                                 */
/* ------------------------------------------------------------------ */

/* Creates a pending future state. The returned state has refcount 1,
 * owned by the caller (normally a Future object that takes ownership). */
void *utopia_future_create(int64_t valueSize, int32_t valueAlign,
                           void (*valueDtor)(void *)) {
  if (valueSize < 0)
    valueSize = 0;
  if (valueAlign <= 0)
    valueAlign = 1;
  uint64_t base = sizeof(UtopiaFutureState);
  uint64_t align = (uint64_t)valueAlign;
  uint64_t off = (base + align - 1) & ~(align - 1);
  auto *s = static_cast<UtopiaFutureState *>(std::malloc(off + (uint64_t)valueSize));
  new (s) UtopiaFutureState();
  s->refs.store(1);
  s->flags.store(0);
  s->head = nullptr;
  s->ownerLoop = nullptr;
  s->valueSize = (uint64_t)valueSize;
  s->valueAlign = (uint32_t)valueAlign;
  s->valueDtor = valueDtor;
  return s;
}

/* Pointer to the value slot inside the state. The compiler uses this to
 * move/copy the future's value in and out with proper move semantics. */
void *utopia_future_value_ptr(void *state) {
  return futureValuePtr(static_cast<UtopiaFutureState *>(state));
}

void utopia_future_retain(void *state) {
  static_cast<UtopiaFutureState *>(state)->refs.fetch_add(1);
}

void utopia_future_release(void *state) {
  if (!state)
    return;
  releaseState(static_cast<UtopiaFutureState *>(state));
}

int utopia_future_is_completed(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  return (s->flags.load(std::memory_order_acquire) & kUtopiaFutureCompleted)
             ? 1
             : 0;
}

/* Marks the state completed (idempotent) and posts every registered
 * continuation to its owner loop. Safe to call from any thread. */
void utopia_future_complete(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  UtopiaContinuation *list = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    if (s->flags.load() & kUtopiaFutureCompleted) {
      return;
    }
    s->flags.store(s->flags.load() | kUtopiaFutureCompleted);
    list = s->head;
    s->head = nullptr;
  }
  /* With no continuations registered yet, keep the value in place: the
   * future can still be awaited later (the await takes the inline path
   * because is_completed is now true). */
  if (!list)
    return;
  UtopiaContinuation *c = list;
  while (c) {
    UtopiaContinuation *next = c->next;
    postContinuation(c);
    c = next;
  }
}

/* Registers a coroutine continuation: when 'state' completes, 'resumeFn'
 * will run with 'frame' on the calling thread's event loop. Retains the
 * state until the continuation has run. */
void utopia_future_then(void *state, void (*resumeFn)(void *frame),
                        void *frame) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->refs.fetch_add(1);
  auto *c = new UtopiaContinuation();
  c->next = nullptr;
  c->loop = getLoop();
  c->resumeFn = resumeFn;
  c->frame = frame;

  bool completed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    if (s->flags.load() & kUtopiaFutureCompleted) {
      completed = true;
    } else {
      c->next = s->head;
      s->head = c;
      return;
    }
  }
  if (completed) {
    /* Completed between the await's check and the registration: the
     * continuation still runs, on the calling loop. */
    c->next = nullptr;
    postContinuation(c);
  }
}

/* ------------------------------------------------------------------ */
/* Callback continuations (Future.then, chaining)                      */
/* ------------------------------------------------------------------ */

struct UtopiaCallbackCont {
  UtopiaFutureState *state; /* retained; released after the callback ran */
  void *cb;                 /* user callback (function pointer) */
  void *thunk;              /* compiler-generated thunk */
  UtopiaFutureState *resultState;
  int isChain;
};

static void postCallbackContinuation(UtopiaCallbackCont *cc) {
  auto *s = cc->state;
  /* The continuation completes resultState later, so it must keep it
   * alive. */
  cc->resultState->refs.fetch_add(1);
  auto *outer = new UtopiaContinuation();
  outer->next = nullptr;
  outer->loop = getLoop();
  outer->frame = cc;
  outer->resumeFn = +[](void *frame) {
    auto *cc2 = static_cast<UtopiaCallbackCont *>(frame);
    UtopiaFutureState *s = cc2->state;
    if (cc2->isChain) {
      utopia_future_complete(cc2->resultState);
    } else {
      typedef void (*ThunkFn)(void *, void *, void *);
      ((ThunkFn)cc2->thunk)(s->valueSize ? futureValuePtr(s) : nullptr,
                            cc2->cb, cc2->resultState);
    }
    releaseState(s);
    releaseState(cc2->resultState);
    delete cc2;
  };

  bool completed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    if (s->flags.load() & kUtopiaFutureCompleted) {
      completed = true;
    } else {
      outer->next = s->head;
      s->head = outer;
      return;
    }
  }
  if (completed)
    postContinuation(outer);
}

/* Wires the compiler-generated 'thunk(valuePtr, cb, resultState)' to a
 * state. Used by Future.then. */
void utopia_future_then_cb(void *state, void *cb, void *thunk,
                           void *resultState) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->refs.fetch_add(1);
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  cc->cb = cb;
  cc->thunk = thunk;
  cc->resultState = static_cast<UtopiaFutureState *>(resultState);
  cc->isChain = 0;
  postCallbackContinuation(cc);
}

/* Chains two states: when 'src' completes, 'dst' is completed too. */
void utopia_future_chain(void *src, void *dst) {
  auto *s = static_cast<UtopiaFutureState *>(src);
  s->refs.fetch_add(1);
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  cc->cb = nullptr;
  cc->thunk = nullptr;
  cc->resultState = static_cast<UtopiaFutureState *>(dst);
  cc->isChain = 1;
  postCallbackContinuation(cc);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

/* Creates a future state that completes after 'us' microseconds on the
 * calling thread's event loop. A non-positive delay completes as soon as
 * the loop's owner next drains: the future is never completed
 * synchronously, so 'await' always suspends and resumes on the event
 * loop (Dart semantics for Future.delayed(Duration.zero, ...)).
 *
 * Timers are deadline entries owned by the calling loop: only its owner
 * expires them (when draining), and there is no global timer thread. */
void *utopia_future_delay_us(int64_t us) {
  void *state = utopia_future_create(0, 1, nullptr);
  UtopiaEventLoop *loop = getLoop();
  auto *t = new UtopiaTimer();
  t->dueUs = nowUs() + (us > 0 ? us : 0);
  t->state = static_cast<UtopiaFutureState *>(state);
  t->next = nullptr;
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    loop->pendingTimers.fetch_add(1);
    UtopiaTimer **walk = &loop->timers;
    while (*walk && (*walk)->dueUs <= t->dueUs)
      walk = &(*walk)->next;
    t->next = *walk;
    *walk = t;
  }
  /* Wake an idle driver: the deadline may be earlier than the one it is
   * currently sleeping toward. */
  loop->cv.notify_all();
  return state;
}

/* Creates a future state that completes after 'ms' milliseconds. */
void *utopia_future_delay(int64_t ms) { return utopia_future_delay_us(ms * 1000); }

/* ------------------------------------------------------------------ */
/* Event loop drivers                                                  */
/* ------------------------------------------------------------------ */

/* Drives the calling thread's loop until 'state' completes. Returns 1 when
 * the future completed, 0 when no pending work exists that could complete
 * it. All resumptions run on the calling thread (strict affinity); the
 * driver merely sleeps when there is nothing to drain yet. */
int utopia_loop_run(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  UtopiaEventLoop *loop = getLoop();
  for (;;) {
    if (utopia_future_is_completed(s))
      return 1;
    if (!hasAnyPendingWork())
      return 0;
    __loop_drain__();
    if (utopia_future_is_completed(s))
      return 1;
    if (loop->queue.empty() && !utopia_future_is_completed(s)) {
      driverIdle(loop);
    }
  }
}

/* Runs all ready tasks on the current thread's loop until no pending work
 * exists (queue empty, no scheduled timers, no running worker threads),
 * mirroring the Dart event loop: a synchronous main that schedules futures
 * keeps the process alive until they all settle. */
void utopia_loop_run_all() {
  UtopiaEventLoop *loop = getLoop();
  for (;;) {
    __loop_drain__();
    if (!hasAnyPendingWork())
      return;
    if (loop->queue.empty()) {
      driverIdle(loop);
    }
  }
}

/* Blocks the calling thread until the state completes. This is an escape
 * hatch for code that is not running an event loop. */
void utopia_future_wait(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  UtopiaEventLoop *loop = getLoop();
  for (;;) {
    if (utopia_future_is_completed(s))
      return;
    if (!hasAnyPendingWork())
      return;
    __loop_drain__();
    if (utopia_future_is_completed(s))
      return;
    if (loop->queue.empty() && !utopia_future_is_completed(s)) {
      driverIdle(loop);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

/* Spawns a worker thread that runs thunk(state, fn). The thunk is
 * compiler-generated and completes the state when the work is done. */
void utopia_thread_spawn(void *state, void *fn, void *thunk) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->ownerLoop = getLoop();
  s->ownerLoop->activeThreads.fetch_add(1);

  auto worker = [s, fn, thunk]() {
    typedef void (*ThunkFn)(void *, void *);
    ((ThunkFn)thunk)(s, fn);
    UtopiaEventLoop *owner = s->ownerLoop;
    owner->activeThreads.fetch_sub(1);
    owner->cv.notify_all();
  };
  std::thread t(worker);
  t.detach();
}

/* Number of hardware threads, exposed for the stdlib. */
int64_t utopia_thread_count() {
  unsigned n = std::thread::hardware_concurrency();
  return n ? (int64_t)n : 1;
}

} /* extern "C" */
