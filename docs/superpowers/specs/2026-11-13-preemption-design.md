# Preemption by retaking the P

## Intent

A fiber that computes for a long time without blocking keeps its P, so
fibers queued behind it, and timers whose fibers land in the queues, starve.
The goal is to avoid that starvation. Matching Go's asynchronous preemption
is not the goal.

## Goals

- When a P has run the same fiber for 10 ms or more and other work is
  waiting, the waiting fibers run within about 20 ms. This holds even if
  the long-running fiber never calls into the runtime: a pure compute loop.
- The long-running fiber is never moved to another thread or switched out
  at an arbitrary instruction. `errno`, `thread_local` values, libc locks
  and OpenSSL's per-thread error queue stay correct.
- Side effect, also wanted: a fiber blocked in a raw system call without
  `blocking_call` (`std::this_thread::sleep_for`, a plain `read`) loses its
  P the same way, as in Go's syscall retake.

## Non-goals

- Bounding busy threads to GOMAXPROCS while long-running fibers run. Each
  fiber whose P was taken keeps its own thread, and the kernel time-slices
  those threads.
- Making a long-running fiber step aside at runtime calls such as channel
  ops or I/O (approach B below). It may come later.
- Preemption on foreign threads (the `is_thread` pseudo-Gs): they have no P.

## Anti-goals

- Any fiber switch from a signal handler.
- A measurable cost on the hot paths (`ready`, `park`, `newproc`): no new
  atomic read-modify-write or fence instructions there, only plain
  per-thread loads and stores, and compiler barriers.

## Approaches

- **A. Retake the P (chosen).** sysmon notices the long-running fiber and
  the P is handed to another M. The fiber keeps running on its thread
  without a P, and gets one back through `exitsyscall` the next time it
  parks, yields or leaves a `blocking_call`. Safe for C++ code. Costs
  threads beyond GOMAXPROCS while long-running fibers run.
- **B. Preemption points the fiber passes through.** A flag checked at
  runtime calls and at an explicit `runtime::PreemptPoint()`. Cheap, but it
  doesn't help pure compute loops. Rejected as the main mechanism; it could
  be added later.
- **C. Go-style asynchronous preemption.** A signal switches fibers at any
  instruction. Rejected: after a stop at an arbitrary instruction and a move
  to another thread, `errno` and `thread_local` addresses are wrong, and
  libc locks can be held across the switch.

The user picked A.

## Design

### Detection (sysmon)

sysmon keeps a private record per P: `{schedtick, schedwhen}`. On each tick,
for each P whose `running` flag is set and that has an M:

- If the P's `schedtick` changed since the last tick, store the new value
  and `now` in the record.
- Otherwise, the P has run the same fiber (or sat in the same blocking raw
  system call) since `schedwhen`. If `now - schedwhen >= 10 ms` and work is
  waiting, ask that M to give up the P (below).

Work is waiting when either holds:

- the P's local queue or `runnext` is not empty, or
- the global queue is not empty and no P is idle.

Late timers are already run by sysmon, and sysmon's polls of the network
put fibers on the global queue, so both of those are covered too.

`P::schedtick`, `P::running` and `P::m` are read by sysmon while their owner
writes them. They become `std::atomic` with relaxed loads and stores; the
owner remains the only writer. sysmon's sleep already backs off to 10 ms at
most, so the time from 10 ms of running to the retake is under about 20 ms.

### Retake request (signal to the M's own thread)

Each M records its kernel thread id (`gettid()`) in `mstart`. To ask for the
P, sysmon:

1. stores the request in the M: the P and the `schedtick` value it saw;
2. sends SIGURG to that thread with `tgkill`.

SIGURG is Go's choice for the same reason: it is ignored by default and
rarely used by programs.

The handler runs on the M's own thread. It reads only that M's fields, so no
cross-thread synchronization is needed beyond the request it reads. It takes
the P only if all of these hold:

- `tls_m` is an M, and its request names the P it holds;
- the P's `schedtick` still equals the request's;
- `m->curg` is set (a fiber is running, not the scheduler context);
- `m->curg->nopreempt == 0` (the fiber is not inside the runtime).

To take it, the handler sets `m->p = nullptr`, `pp->m = nullptr` and
`pp->running = false`, then publishes the P to sysmon (`m->retaken`, a
release store). The handler does nothing else: no locks, no allocation, no
fiber switch. sysmon picks up `retaken` (exchange with `nullptr`) and starts
an M on it with `startm(pp, false)`. sysmon may spin briefly for the
acknowledgement or pick it up on its next tick; that choice is left to the
builder. A handler that declines just clears the request.

`errno` is saved and restored, as `sighandler` already does.

### The `nopreempt` bracket

The handler must never take the P while fiber code is between reading
`m->p` and using it: `runqput` assumes a single owner. Every runtime path
that reads `m->p` from a fiber therefore runs inside a bracket:

- a counter `G::nopreempt`, incremented on entry and decremented on exit,
- with `std::atomic_signal_fence` after the increment and before the
  decrement.

The counter lives on the G, not the M. A fiber that parks inside the bracket
(in `park`, `Gosched`, `exitsyscall`) resumes later, possibly on another M,
and finishes the bracket there.

Today only `proc.cpp` reads `m->p`. The bracket goes on these entry points:

- `newproc`, `ready`, `injectglist` when called from a fiber,
- `park`, `Gosched`, `entersyscall`, `exitsyscall`, `has_runnable`,
- the exit path in `gentry`.

The invariants the builder keeps:

- A fiber switches to the scheduler context only from inside a bracket.
  `gentry` sets it before returning.
- The scheduler context in `execute` reads `m->p` only while `m->curg` is
  null. So `schedtick++` comes before `m->curg = gp`, and `m->curg =
  nullptr` comes right after the resume returns. Both are separated from
  the `m->p` accesses by signal fences.

### After the P was taken

The fiber keeps running with `m->p == nullptr`. Paths that already handle a
P-less M take over:

- `ready` and `newproc` use the global queue;
- `entersyscall` returns early;
- `park` and `Gosched` call `exitsyscall`, which takes an idle P or queues
  the fiber and stops the M.

The number of Ms grows by at most one per long-running fiber whose P was
taken. Ms are reused through `midle` as today.

### Signals and os/signal

- `initsig` installs the runtime handler for SIGURG at startup, with
  `SA_RESTART | SA_ONSTACK`.
- `sighandler` on SIGURG does the preemption check first. Then, as for
  every signal, it calls `sigsend`, or `forward` if os/signal didn't ask
  for it, so `Notify(SIGURG)` keeps working (`tests/go_os_signal_test.cpp`
  already expects stray SIGURGs).
- `signal_disable` and `signal_ignore` on SIGURG update os/signal's
  bookkeeping but leave the runtime handler installed. Go does the same.
- System calls that `SA_RESTART` doesn't restart (`nanosleep`,
  `epoll_wait`, `poll`) can return `EINTR` in user code that made them
  without `blocking_call`. The same is true in Go; the README says so.

### Off switch

`GODEBUG=asyncpreemptoff=1`, Go's name for the same switch, turns off the
retake in sysmon. This was my call: it is cheap and helps when debugging.

### Observability

`GOPP_SCHEDTRACE` gains a `retake` count, kept as a global atomic like
`sysmon_polls`, since sysmon has no M.

## Tests (in `tests/runtime_test.cpp`)

1. With GOMAXPROCS=1, a fiber spins in a loop with no calls for 500 ms.
   Another fiber's `time::Sleep(10ms)` returns within 100 ms, and a
   ping-pong over a channel completes during the spin.
2. With every P running a spinning fiber, a fiber started with `go` runs
   within 100 ms.
3. A fiber whose P was taken then parks on a channel, is woken, and goes on
   correctly with a P again.
4. A fiber blocked in raw `::nanosleep` or `read` loses its P, and queued
   fibers run. Pick whichever call doesn't make the test flaky through
   `EINTR`, or handle `EINTR` in the test.
5. `Notify(SIGURG)` still delivers an externally sent SIGURG, and the
   os/signal tests still pass.
6. With `GODEBUG=asyncpreemptoff=1`, test 1's sleeper is not woken during
   the spin. This can be a subprocess test, or it can be skipped if
   awkward.

Also run the HTTP benchmark (`jolt build benchmark`) before and after. The
bracket must not move req/s or p99 beyond run-to-run noise.

## Docs

Replace README's "No preemption" bullet. Say that a fiber computing for
over 10 ms while others wait loses its P to another thread, and is not
switched out, so it keeps its thread. Mention the `EINTR` caveat and
`GODEBUG=asyncpreemptoff=1`.

## Left to the builder

- Whether sysmon spins for the handler's acknowledgement or picks it up on
  the next tick.
- The exact fence placement in `execute` and `mcall`, keeping the
  invariants above.
- Test timings and how much slack they need so they aren't flaky on a
  loaded machine.
