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
 * - The program is driven by utopia_loop_run (async main) or
 *   utopia_loop_drain (sync main), which run ready tasks until the target
 *   future completes or there is no pending work left.
 *
 * Threads
 * -------
 * - Future.runOnThread spawns a worker thread (utopia_thread_spawn). The
 *   worker runs a compiler-generated thunk that calls the user function
 *   and completes the future state; the completion is posted back to the
 *   owner loop, so `await` on it blocks the loop until the thread finishes.
 * - A global timer thread powers Future.delayed.
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

struct UtopiaContinuation {
  UtopiaContinuation *next;
  struct UtopiaEventLoop *loop;
  void (*resumeFn)(void *frame);
  void *frame;
};

struct UtopiaEventLoop {
  std::mutex mtx;
  std::condition_variable cv;
  std::deque<UtopiaContinuation *> queue;
  std::atomic<int32_t> activeThreads{0};
  std::atomic<int32_t> pendingTimers{0};
  /* Monotonic timestamp of the last time the loop's owner pumped it. The
   * background scheduler takes over a loop whose owner has not pumped for
   * kSchedulerTakeoverMs: this keeps resumptions on the owning thread while
   * it is actively driving (Dart-like single-thread behavior) yet lets
   * fire-and-forget work proceed when the owner is blocked in user code
   * (e.g. a game loop) or inside a coroutine ramp. */
  std::atomic<int64_t> lastPumpMs{0};
  /* Global registry link (guarded by gLoopRegistryMtx). */
  UtopiaEventLoop *next = nullptr;
};

struct UtopiaFutureState {
  std::atomic<int32_t> refs;
  std::atomic<int32_t> flags; /* bit 0: completed */
  std::mutex mtx;             /* guards head + flags */
  UtopiaContinuation *head;
  UtopiaEventLoop *ownerLoop;
  uint64_t valueSize;
  uint32_t valueAlign;
  void (*valueDtor)(void *valuePtr);
  /* The value storage follows the struct, aligned to valueAlign. */
};

enum : uint32_t { kUtopiaFutureCompleted = 1u };

static thread_local UtopiaEventLoop *tlsLoop = nullptr;

/* ------------------------------------------------------------------ */
/* Background scheduler                                                */
/*                                                                     */
/* A detached thread that pumps every event loop whose owner has not   */
/* pumped it recently (kSchedulerTakeoverMs). This gives Dart/Flutter  */
/* semantics to fire-and-forget futures when the main thread is        */
/* blocked in user code (e.g. a game loop): timers and thread          */
/* completions still run their continuations in the background instead */
/* of waiting for the program to exit.                                 */
/* ------------------------------------------------------------------ */

static std::mutex gLoopRegistryMtx;
static UtopiaEventLoop *gLoops = nullptr;

/* Serializes runPending across all threads (owner pumps + scheduler). */
static std::mutex gPumpMtx;

static std::mutex gSchedMtx;
static std::condition_variable gSchedCv;
static std::atomic<bool> gSchedStarted{false};
static std::thread *gSchedThread = nullptr;

static constexpr int64_t kSchedulerTakeoverMs = 10;

static void schedulerMain();
static void runPending(UtopiaEventLoop *loop);

static void startScheduler() {
  if (gSchedStarted.load())
    return;
  bool expected = false;
  if (gSchedStarted.compare_exchange_strong(expected, true)) {
    gSchedThread = new std::thread(schedulerMain);
    gSchedThread->detach();
  }
}

static void registerLoop(UtopiaEventLoop *loop) {
  std::lock_guard<std::mutex> lock(gLoopRegistryMtx);
  loop->next = gLoops;
  gLoops = loop;
  startScheduler();
}

static UtopiaEventLoop *getLoop() {
  if (!tlsLoop) {
    tlsLoop = new UtopiaEventLoop();
    registerLoop(tlsLoop);
  }
  return tlsLoop;
}

static int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

static void schedulerMain() {
  for (;;) {
    bool didWork = false;
    UtopiaEventLoop *it;
    {
      std::lock_guard<std::mutex> lock(gLoopRegistryMtx);
      it = gLoops;
    }
    while (it) {
      /* Take over loops whose owner has not pumped them recently. */
      if (nowMs() - it->lastPumpMs.load() > kSchedulerTakeoverMs) {
        bool hasWork = false;
        {
          std::lock_guard<std::mutex> lock(it->mtx);
          hasWork = !it->queue.empty();
        }
        if (hasWork) {
          runPending(it);
          /* Wake loop drivers sleeping on the global cv so they can
           * re-check their target future. */
          {
            std::lock_guard<std::mutex> lock(gSchedMtx);
            gSchedCv.notify_all();
          }
          didWork = true;
        }
      }
      it = it->next;
    }
    if (!didWork) {
      std::unique_lock<std::mutex> lock(gSchedMtx);
      gSchedCv.wait_for(lock, std::chrono::milliseconds(5));
    }
  }
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
  /* The background scheduler may need to pick this up when the owner is
   * blocked. */
  {
    std::lock_guard<std::mutex> lock(gSchedMtx);
    gSchedCv.notify_all();
  }
}

/* Runs every ready task on 'loop'. The queue pop is serialized globally so
 * the owner thread and the background scheduler never pop the same
 * continuation, but the continuation itself runs without any lock: a
 * blocking continuation (e.g. a game loop) must not stall the other
 * pumpers. */
static void runPending(UtopiaEventLoop *loop) {
  for (;;) {
    UtopiaContinuation *c = nullptr;
    {
      std::lock_guard<std::mutex> pump(gPumpMtx);
      std::lock_guard<std::mutex> lock(loop->mtx);
      if (!loop->queue.empty()) {
        c = loop->queue.front();
        loop->queue.pop_front();
      }
    }
    if (!c)
      break;
    c->resumeFn(c->frame);
    delete c;
  }
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

extern "C" {

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
    std::lock_guard<std::mutex> lock(s->mtx);
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
    std::lock_guard<std::mutex> lock(s->mtx);
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
    std::lock_guard<std::mutex> lock(s->mtx);
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

struct UtopiaTimer {
  int64_t dueMs;
  UtopiaFutureState *state;
  UtopiaTimer *next;
};

/* The timer globals are intentionally heap-allocated and never destroyed:
 * the detached timer thread may still be using them when the process exits,
 * and destroying a condition_variable that a thread is waiting on hangs. */
static std::mutex &gTimerMtx() {
  static auto *m = new std::mutex();
  return *m;
}
static UtopiaTimer *&gTimers() {
  static auto *t = new UtopiaTimer *();
  return *t;
}
static std::condition_variable &gTimerCv() {
  static auto *cv = new std::condition_variable();
  return *cv;
}
static std::thread &gTimerThread() {
  static auto *th = new std::thread();
  return *th;
}
static std::atomic<bool> gTimerStarted{false};

static void timerThreadMain() {
  std::unique_lock<std::mutex> lock(gTimerMtx());
  for (;;) {
    if (!gTimers()) {
      gTimerCv().wait(lock);
      continue;
    }
    int64_t now = nowMs();
    if (gTimers()->dueMs > now) {
      gTimerCv().wait_for(lock,
                          std::chrono::milliseconds(gTimers()->dueMs - now));
      continue;
    }
    UtopiaTimer *due = gTimers();
    gTimers() = due->next;
    UtopiaFutureState *s = due->state;
    UtopiaEventLoop *owner = s->ownerLoop;
    delete due;
    lock.unlock();
    utopia_future_complete(s);
    if (owner) {
      std::lock_guard<std::mutex> lock2(owner->mtx);
      owner->pendingTimers.fetch_sub(1);
    }
    owner->cv.notify_all();
    {
      std::lock_guard<std::mutex> lockSched(gSchedMtx);
      gSchedCv.notify_all();
    }
    lock.lock();
  }
}

/* ------------------------------------------------------------------ */
/* Event loop drivers                                                  */
/* ------------------------------------------------------------------ */

/* Drives the loop until 'state' completes. Returns 1 when the future
 * completed, 0 when no pending work exists that could complete it. The
 * owner pumps its own loop so resumptions stay on the calling thread
 * whenever possible; the background scheduler takes over automatically
 * whenever the owner stalls (e.g. a blocking game loop). */
/* Drives the loop until 'state' completes. Returns 1 when the future
 * completed, 0 when no pending work exists that could complete it. The
 * owner pumps its own loop so resumptions stay on the calling thread
 * whenever possible; the background scheduler takes over automatically
 * whenever the owner stalls (e.g. a blocking game loop).
 *
 * The keep-alive semantics (stay alive while fire-and-forget work is still
 * pending after the target completed) are handled by utopia_loop_run_all,
 * which the async-main wrapper calls afterwards: a driver must return as
 * soon as its own target completes, otherwise a worker driving its own
 * future would wait forever on its own 'activeThreads' contribution. */
int utopia_loop_run(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  UtopiaEventLoop *loop = getLoop();
  for (;;) {
    if (utopia_future_is_completed(s))
      return 1;
    if (!hasAnyPendingWork())
      return 0;
    loop->lastPumpMs.store(nowMs());
    runPending(loop);
    if (loop->queue.empty()) {
      std::unique_lock<std::mutex> lock(gSchedMtx);
      if (loop->queue.empty() && hasAnyPendingWork()) {
        gSchedCv.wait_for(lock, std::chrono::milliseconds(5));
      }
    }
  }
}

/* Runs all ready tasks on the current thread's loop. Used after a
 * synchronous main returns so futures completed during main get to run
 * their continuations. */
void utopia_loop_drain() { runPending(getLoop()); }

/* Drives the calling thread's event loop until no pending work exists
 * (queue empty, no scheduled timers, no running worker threads), mirroring
 * the Dart event loop: a synchronous main that schedules futures keeps the
 * process alive until they all settle. */
void utopia_loop_run_all() {
  UtopiaEventLoop *loop = getLoop();
  for (;;) {
    loop->lastPumpMs.store(nowMs());
    runPending(loop);
    if (!hasAnyPendingWork())
      return;
    std::unique_lock<std::mutex> lock(gSchedMtx);
    if (loop->queue.empty() && hasAnyPendingWork()) {
      gSchedCv.wait_for(lock, std::chrono::milliseconds(5));
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
    loop->lastPumpMs.store(nowMs());
    runPending(loop);
    if (utopia_future_is_completed(s))
      return;
    if (loop->queue.empty()) {
      std::unique_lock<std::mutex> lock(gSchedMtx);
      if (loop->queue.empty() && !utopia_future_is_completed(s)) {
        if (hasAnyPendingWork()) {
          gSchedCv.wait_for(lock, std::chrono::milliseconds(5));
        } else {
          return;
        }
      }
    }
  }
}

/* Creates a future state that completes after 'ms' milliseconds on the
 * calling thread's event loop. */
void *utopia_future_delay(int64_t ms) {
  void *state = utopia_future_create(0, 1, nullptr);
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->ownerLoop = getLoop();
  s->ownerLoop->pendingTimers.fetch_add(1);

  if (!gTimerStarted.load()) {
    bool expected = false;
    if (gTimerStarted.compare_exchange_strong(expected, true)) {
      gTimerThread() = std::thread(timerThreadMain);
      gTimerThread().detach();
    }
  }
  auto *t = new UtopiaTimer();
  t->dueMs = nowMs() + (ms > 0 ? ms : 0);
  t->state = s;
  t->next = nullptr;
  {
    std::lock_guard<std::mutex> lock(gTimerMtx());
    UtopiaTimer **walk = &gTimers();
    while (*walk && (*walk)->dueMs <= t->dueMs)
      walk = &(*walk)->next;
    t->next = *walk;
    *walk = t;
  }
  gTimerCv().notify_one();
  return state;
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
