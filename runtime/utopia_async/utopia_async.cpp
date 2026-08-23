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
#include <cstdio>
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
  /* Either a state to complete (utopia_future_delay_us) or a callback
   * registration to run (utopia_future_timer_cb). Exactly one of the two
   * forms is active. */
  UtopiaFutureState *state;
  void *cb;
  void *thunk;
  UtopiaFutureState *resultState;
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

/* The error value of a failed future: a heap copy of the thrown value plus
 * its descriptor and destructor. The copy is what lets 'await' rethrow the
 * same dynamic type after the original exception record is gone. */
struct UtopiaError {
  uint64_t valueSize;
  uint32_t valueAlign;
  void *typeInfo;
  void (*valueDtor)(void *);
  /* The value storage follows the struct, aligned to valueAlign. */
};

struct UtopiaFutureState {
  std::atomic<int32_t> refs;
  std::atomic<int32_t> flags; /* bit 0: completed, bit 1: error, bit 2: observed */
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
  /* Set when the future completed with an error; owns the error copy until
   * the state is released. */
  UtopiaError *error = nullptr;
  /* The value storage follows the struct, aligned to valueAlign. */
};

enum : uint32_t {
  kUtopiaFutureCompleted = 1u,
  kUtopiaFutureError = 2u,
  kUtopiaFutureErrorObserved = 4u,
};

static thread_local UtopiaEventLoop *tlsLoop = nullptr;

extern "C" void utopia_future_complete(void *state);

/* Exception-record accessors from utopia_runtime. The async runtime never
 * inspects the record layout: it copies the thrown value out through these
 * exports and disposes the record when the copy is done. */
extern "C" void utopia_exception_info(void *exnPtr, void **outTypeInfo,
                                      void **outValuePtr,
                                      uint64_t *outValueSize,
                                      void (**outDtor)(void *));
extern "C" void utopia_exception_dispose(void *exnPtr);
extern "C" void *utopia_allocate_exception(uint64_t size);
extern "C" void utopia_throw(void *valuePtr, void *typeInfo,
                             void (*dtor)(void *));

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

static void *errorValuePtr(UtopiaError *e) {
  uint64_t base = sizeof(UtopiaError);
  uint64_t align = e->valueAlign ? e->valueAlign : 1;
  uint64_t off = (base + align - 1) & ~(align - 1);
  return reinterpret_cast<char *>(e) + off;
}

/* Destroys a future's error copy. An unobserved error (never rethrown by an
 * await, never handed to an error handler) is reported like the sync
 * runtime's unhandled-exception path: it is a programming error, and
 * silently swallowing it would hide broken fire-and-forget futures. */
static void destroyError(UtopiaError *e) {
  if (e->valueSize && e->valueDtor)
    e->valueDtor(errorValuePtr(e));
  std::free(e);
}

static void releaseState(UtopiaFutureState *s) {
  if (s->refs.fetch_sub(1) == 1) {
    if (s->error) {
      if (!(s->flags.load(std::memory_order_acquire) &
            kUtopiaFutureErrorObserved)) {
        std::fputs("Unhandled async error: a future completed with an error "
                   "that no 'await', 'catchError' or 'onError' handler "
                   "observed.\n",
                   stderr);
        std::abort();
      }
      destroyError(s->error);
    }
    if (s->valueSize && s->valueDtor)
      s->valueDtor(futureValuePtr(s));
    delete s;
  }
}

static UtopiaError *copyErrorInto(UtopiaFutureState *dst,
                                  const UtopiaError *src) {
  uint64_t align = src->valueAlign ? src->valueAlign : 1;
  uint64_t base = sizeof(UtopiaError);
  uint64_t off = (base + align - 1) & ~(align - 1);
  auto *e =
      static_cast<UtopiaError *>(std::malloc(off + src->valueSize));
  e->valueSize = src->valueSize;
  e->valueAlign = src->valueAlign;
  e->typeInfo = src->typeInfo;
  e->valueDtor = src->valueDtor;
  if (src->valueSize)
    std::memcpy(errorValuePtr(e),
                errorValuePtr(const_cast<UtopiaError *>(src)),
                src->valueSize);
  return e;
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
  std::vector<UtopiaTimer *> dueCallbacks;
  {
    std::lock_guard<std::mutex> lock(loop->mtx);
    while (loop->timers && loop->timers->dueUs <= now) {
      UtopiaTimer *t = loop->timers;
      loop->timers = t->next;
      loop->pendingTimers.fetch_sub(1);
      if (t->thunk) {
        dueCallbacks.push_back(t);
        continue;
      }
      due.push_back(t->state);
      delete t;
    }
  }
  for (UtopiaFutureState *s : due)
    utopia_future_complete(s);
  /* Callback timers carry a retained resultState; the thunk completes it
   * and the release here balances the retain made at registration. */
  for (UtopiaTimer *t : dueCallbacks) {
    typedef void (*TimerThunkFn)(void *, void *);
    ((TimerThunkFn)t->thunk)(t->cb, t->resultState);
    releaseState(t->resultState);
    delete t;
  }
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

int utopia_future_has_error(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  return (s->flags.load(std::memory_order_acquire) & kUtopiaFutureError) ? 1
                                                                         : 0;
}

/* Completes a future with an error. 'typeInfo' identifies the thrown type,
 * 'valuePtr' points at the thrown value (copied into a private slot owned
 * by the state), 'valueSize'/'valueAlign' describe the value and 'dtor'
 * destroys it (or null). The caller keeps ownership of its own storage and
 * must free it. Used by the compiler's error thunks (e.g. a thrown
 * TimeoutException) and by the error-forwarding helpers below. */
void utopia_future_complete_error(void *state, void *typeInfo, void *valuePtr,
                                  int64_t valueSize, int32_t valueAlign,
                                  void (*dtor)(void *)) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  if (valueSize < 0)
    valueSize = 0;
  if (valueAlign <= 0)
    valueAlign = 1;
  uint64_t base = sizeof(UtopiaError);
  uint64_t align = (uint64_t)valueAlign;
  uint64_t off = (base + align - 1) & ~(align - 1);
  auto *e = static_cast<UtopiaError *>(std::malloc(off + (uint64_t)valueSize));
  e->valueSize = (uint64_t)valueSize;
  e->valueAlign = (uint32_t)valueAlign;
  e->typeInfo = typeInfo;
  e->valueDtor = dtor;
  if (valueSize)
    std::memcpy(errorValuePtr(e), valuePtr, (uint64_t)valueSize);

  UtopiaContinuation *list = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    if (s->flags.load() & kUtopiaFutureCompleted) {
      /* Already completed (e.g. the timeout timer fired after the source
       * won the race): drop the copy and keep the first completion. */
      destroyError(e);
      return;
    }
    s->error = e;
    s->flags.store(s->flags.load() | kUtopiaFutureCompleted |
                   kUtopiaFutureError);
    list = s->head;
    s->head = nullptr;
  }
  if (!list)
    return;
  UtopiaContinuation *c = list;
  while (c) {
    UtopiaContinuation *next = c->next;
    postContinuation(c);
    c = next;
  }
}

/* Completes the future with the exception being raised at the boundary of
 * an async function (the exception that no catch clause inside the
 * coroutine handled). The record's value is copied out and the record is
 * disposed, so the boundary handler runs exactly like a regular catch. */
void utopia_future_complete_error_exn(void *state, void *exnPtr) {
  void *typeInfo = nullptr;
  void *valuePtr = nullptr;
  uint64_t valueSize = 0;
  void (*dtor)(void *) = nullptr;
  utopia_exception_info(exnPtr, &typeInfo, &valuePtr, &valueSize, &dtor);
  utopia_future_complete_error(state, typeInfo, valuePtr, (int64_t)valueSize,
                               0, dtor);
  utopia_exception_dispose(valuePtr);
}

/* Copies the error of 'src' into 'dst' (which must be pending) and
 * completes it. Both states must be created with compatible value
 * layouts; the runtime simply mirrors the stored bytes. */
void utopia_future_forward_error(void *dst, void *src) {
  auto *s = static_cast<UtopiaFutureState *>(src);
  UtopiaError *copy = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    if (s->error) {
      copy = copyErrorInto(static_cast<UtopiaFutureState *>(dst), s->error);
    }
  }
  if (copy) {
    utopia_future_complete_error(dst, copy->typeInfo, errorValuePtr(copy),
                                 (int64_t)copy->valueSize, copy->valueAlign,
                                 copy->valueDtor);
    std::free(copy);
  }
}

/* Rethrows the error stored in the state (the 'await' side of a failed
 * future). A fresh exception record is raised so the dynamic type is
 * preserved and the state keeps its copy: the same future can be awaited
 * again and rethrows again. Does not return; a state without an error is a
 * compiler bug and terminates. */
void utopia_future_rethrow(void *state) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  UtopiaError *e = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    e = s->error;
    s->flags.store(s->flags.load() | kUtopiaFutureErrorObserved);
  }
  if (!e) {
    std::fputs("utopia_future_rethrow on a future without an error.\n",
               stderr);
    std::abort();
  }
  void *fresh = utopia_allocate_exception(e->valueSize);
  if (e->valueSize)
    std::memcpy(fresh, errorValuePtr(e), e->valueSize);
  utopia_throw(fresh, e->typeInfo, e->valueDtor);
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
/* Callback continuations (Future.then, error handlers, chaining)      */
/* ------------------------------------------------------------------ */

struct UtopiaCallbackCont {
  UtopiaFutureState *state; /* retained; released after the callback ran */
  void *cb;                 /* user callback (function pointer) */
  void *thunk;              /* compiler-generated thunk */
  void *errCb;              /* error callback, or null when none */
  void *errThunk;           /* compiler-generated error thunk, or null */
  UtopiaFutureState *resultState;
  int isChain;   /* chain: complete dst when src completes */
  int isForward; /* forward: copy src's value/error into dst */
  /* Run the value thunk even when src completed with an error (used by
   * Future.whenComplete, which must run its callback either way). */
  int runOnError;
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
    bool hasError = (s->flags.load(std::memory_order_acquire) &
                     kUtopiaFutureError) != 0;
    if (hasError) {
      /* A failed source never runs the value callback: the error is handed
       * to the error thunk, forwarded to the result future, or (for
       * whenComplete) the value thunk still runs but the error is then
       * forwarded. */
      if (cc2->errThunk) {
        typedef void (*ErrThunkFn)(void *, void *, void *);
        ((ErrThunkFn)cc2->errThunk)(
            s->error ? errorValuePtr(s->error) : nullptr, cc2->errCb,
            cc2->resultState);
        s->flags.store(s->flags.load() | kUtopiaFutureErrorObserved);
      } else if (cc2->runOnError) {
        typedef void (*ThunkFn)(void *, void *, void *);
        ((ThunkFn)cc2->thunk)(nullptr, cc2->cb, cc2->resultState);
        utopia_future_forward_error(cc2->resultState, s);
        s->flags.store(s->flags.load() | kUtopiaFutureErrorObserved);
      } else {
        utopia_future_forward_error(cc2->resultState, s);
        s->flags.store(s->flags.load() | kUtopiaFutureErrorObserved);
      }
    } else if (cc2->isChain) {
      utopia_future_complete(cc2->resultState);
    } else if (cc2->isForward) {
      auto *d = cc2->resultState;
      if (s->valueSize) {
        std::lock_guard<std::recursive_mutex> lock(d->mtx);
        std::memcpy(futureValuePtr(d), futureValuePtr(s), s->valueSize);
      }
      utopia_future_complete(d);
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
 * state. Used by Future.then and Future.whenComplete. */
void utopia_future_then_cb(void *state, void *cb, void *thunk,
                           void *resultState) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->refs.fetch_add(1);
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  cc->cb = cb;
  cc->thunk = thunk;
  cc->errCb = nullptr;
  cc->errThunk = nullptr;
  cc->resultState = static_cast<UtopiaFutureState *>(resultState);
  cc->isChain = 0;
  cc->isForward = 0;
  cc->runOnError = 0;
  postCallbackContinuation(cc);
}

/* Variant with an error handler: 'errThunk(errValuePtr, errCb,
 * resultState)' runs when the source completes with an error, and the
 * value thunk never does. Used by Future.catchError and the onError
 * parameter of Future.then. On success the source's value is forwarded
 * into resultState. */
void utopia_future_then_cb_err(void *state, void *cb, void *thunk,
                               void *errCb, void *errThunk,
                               void *resultState) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->refs.fetch_add(1);
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  cc->cb = cb;
  cc->thunk = thunk;
  cc->errCb = errCb;
  cc->errThunk = errThunk;
  cc->resultState = static_cast<UtopiaFutureState *>(resultState);
  cc->isChain = 0;
  cc->isForward = 1;
  cc->runOnError = 0;
  postCallbackContinuation(cc);
}

/* whenComplete-style registration: the value thunk always runs (with a
 * null value pointer on the error path) and the source error, if any, is
 * then forwarded to resultState, so the callback observes the completion
 * without swallowing the error (Dart semantics). */
void utopia_future_then_cb_when(void *state, void *cb, void *thunk,
                                void *resultState) {
  auto *s = static_cast<UtopiaFutureState *>(state);
  s->refs.fetch_add(1);
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  cc->cb = cb;
  cc->thunk = thunk;
  cc->errCb = nullptr;
  cc->errThunk = nullptr;
  cc->resultState = static_cast<UtopiaFutureState *>(resultState);
  cc->isChain = 0;
  cc->isForward = 0;
  cc->runOnError = 1;
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
  cc->errCb = nullptr;
  cc->errThunk = nullptr;
  cc->resultState = static_cast<UtopiaFutureState *>(dst);
  cc->isChain = 1;
  cc->isForward = 0;
  cc->runOnError = 0;
  postCallbackContinuation(cc);
}

/* Forwards the completion of 'src' into 'dst': the value is copied
 * byte-wise (only valid for states created with the same value type), or
 * the error is forwarded when src failed. dst completes exactly once,
 * whichever side wins (used by Future.timeout). */
void utopia_future_forward(void *dst, void *src) {
  auto *s = static_cast<UtopiaFutureState *>(src);
  bool alreadyError = false;
  {
    std::lock_guard<std::recursive_mutex> lock(s->mtx);
    alreadyError = (s->flags.load() & kUtopiaFutureError) != 0;
    if (s->flags.load() & kUtopiaFutureCompleted) {
      auto *d = static_cast<UtopiaFutureState *>(dst);
      if (s->valueSize && !alreadyError) {
        std::lock_guard<std::recursive_mutex> dlock(d->mtx);
        std::memcpy(futureValuePtr(d), futureValuePtr(s), s->valueSize);
      }
      utopia_future_complete(dst);
      return;
    }
  }
  if (alreadyError) {
    utopia_future_forward_error(dst, src);
    return;
  }
  /* Still pending: register a forwarding continuation. The source must be
   * retained like every other continuation source: the continuation
   * releases it after it runs, and without the retain the source could be
   * freed first and released again from the continuation (UAF). */
  auto *cc = new UtopiaCallbackCont();
  cc->state = s;
  s->refs.fetch_add(1);
  cc->cb = nullptr;
  cc->thunk = nullptr;
  cc->errCb = nullptr;
  cc->errThunk = nullptr;
  cc->resultState = static_cast<UtopiaFutureState *>(dst);
  cc->isChain = 0;
  cc->isForward = 1;
  cc->runOnError = 0;
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
  /* The callback fields must be null for the plain-state form: expireTimers
   * distinguishes the two timer kinds by t->thunk, and a garbage pointer
   * would be invoked as a callback thunk. */
  t->cb = nullptr;
  t->thunk = nullptr;
  t->resultState = nullptr;
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

/* Schedules a callback to run after 'us' microseconds on the calling
 * thread's event loop. When it fires, 'thunk(cb, resultState)' runs and
 * the thunk completes resultState (Dart's Future.delayed with a callback,
 * Future.timeout's deadline and Future.microtask all lower to this). The
 * result state is retained until the timer fires, so the future stays
 * alive even if the caller drops it. */
void utopia_future_timer_cb(int64_t us, void *cb, void *thunk,
                            void *resultState) {
  UtopiaEventLoop *loop = getLoop();
  auto *t = new UtopiaTimer();
  t->dueUs = nowUs() + (us > 0 ? us : 0);
  t->state = nullptr;
  t->cb = cb;
  t->thunk = thunk;
  t->resultState = static_cast<UtopiaFutureState *>(resultState);
  t->resultState->refs.fetch_add(1);
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
  loop->cv.notify_all();
}

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
