# Preemption by Retaking the P: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fiber that runs for 10 ms or more while other work waits loses
its P to another thread, so queued fibers and timers don't starve.

**Architecture:** sysmon detects a P whose `schedtick` hasn't moved for
10 ms while work is waiting. It sends SIGURG to that M's thread. The
handler, on that same thread, detaches the P when the fiber is outside the
runtime, and sysmon starts an M on it. Runtime code that reads `m->p` from
a fiber runs inside a per-G `nopreempt` bracket. The long-running fiber
keeps running without a P and gets one back through `exitsyscall`.

**Tech Stack:** C++26 (GCC 16), Boost.Context fibers, Linux signals
(`tgkill`), GoogleTest, jolt.

**Spec:** `docs/superpowers/specs/2026-11-13-preemption-design.md`

## Global Constraints

- Linux x86-64 is the target. Keep the code ARM64-clean: no
  architecture-specific code is needed.
- The signal is SIGURG.
- The retake threshold is 10 ms (`constexpr int64_t force_preempt = 10'000'000;`, in ns).
- The off switch is `GODEBUG` containing `asyncpreemptoff=1`.
- The signal handler takes no locks, allocates nothing and never switches
  fibers.
- The hot paths (`ready`, `park`, `newproc`, `runqput`) gain no atomic
  read-modify-write or fence instructions. Only plain loads and stores and
  `std::atomic_signal_fence` are allowed there.
- Work on branch `preemption`. Use straight history: no merge commits, and
  rebase onto `master`. Commit subjects follow the repo style
  (`runtime: ...`).
- Build and test commands:
  - all tests: `jolt build test`
  - filtered: `jolt build "test:filter=Runtime.*"`
  - benchmark: `jolt build benchmark` (writes `report/benchmark.md`)

## Review Focus

1. **A fiber that loses its P while inside `ready`/`runqput`.** The
   bracket must make this impossible. Without it, run queues get
   corrupted, which shows up as lost fibers or a crash. Exercised by Task
   3's `PreemptUnderChurn` test.
2. **A retake landing in `execute` between the resume and
   `m->curg = nullptr`.** The handler must decline; otherwise there is a
   null `m->p` dereference in the scheduler. Exercised by the same
   churn test.
3. **A fiber whose P was taken, then parks and resumes.** It must get a P
   back and keep working, and the M count must not grow without bound.
   Tested by `PreemptedFiberParksAndResumes`.
4. **`Notify(SIGURG)` after preemption is installed.** It must still
   deliver externally sent SIGURGs. `Stop` and `Ignore` must not remove
   the handler. Tested in Task 1.
5. **User code with raw blocking system calls.** It loses its P and
   queued fibers run. `EINTR` from SIGURG may reach such code; the test
   tolerates it. Tested by `RawBlockingSyscallLosesP`.

---

### Task 1: SIGURG handler installed at startup and kept for the runtime

**Files:**
- Modify: `src/runtime/signal.cpp` (`initsig`, `sighandler`, `signal_disable`, `signal_ignore`, `sig_state`)
- Modify: `src/runtime/runtime.hpp` (declare the hook)
- Modify: `src/runtime/proc.cpp` (stub hook)
- Test: `tests/go_os_signal_test.cpp`

**Interfaces:**
- Produces: `void preempt_signal() noexcept;` in `gopp::runtime`. It is
  declared in `runtime.hpp` and defined in `proc.cpp`, as an empty stub in
  this task. It is async-signal-safe, and `sighandler` calls it first for
  SIGURG.
- Produces: `sig_state::sigurg_at_init` (bool). While set,
  `signal_disable` and `signal_ignore` leave the runtime handler
  installed for SIGURG.

- [ ] **Step 1: Write the failing test** `TEST(GoOsSignal, SIGURGHandlerStaysInstalled)`

```cpp
auto installed = [] {
    struct sigaction sa{};
    ::sigaction(SIGURG, nullptr, &sa);
    return (sa.sa_flags & SA_SIGINFO) != 0;
};
EXPECT_TRUE(installed());                 // from startup
auto c = make_chan<os::Signal>(1);
signal::Notify(c, syscall::SIGURG_);
::kill(::getpid(), SIGURG);
EXPECT_EQ(c.recv(), os::Signal(syscall::SIGURG_));
signal::Stop(c);
EXPECT_TRUE(installed());                 // Stop keeps it
signal::Ignore(syscall::SIGURG_);
EXPECT_TRUE(installed());                 // Ignore keeps it
EXPECT_TRUE(signal::Ignored(syscall::SIGURG_));
signal::Reset(syscall::SIGURG_);
EXPECT_TRUE(installed());
```

Match the file's existing names for `Notify`, `Stop`, `Ignore`,
`Ignored`, `Reset` and the signal constant. Use `wait_sig` if a direct
`recv` races with stray SIGURGs.

- [ ] **Step 2: Run it and check that it fails**

Run: `jolt build "test:filter=GoOsSignal.SIGURGHandlerStaysInstalled"`
Expected: FAIL on the first `installed()`.

- [ ] **Step 3: Implement**

- In `initsig`, install the runtime handler for SIGURG unconditionally,
  even if it was ignored at start, as Go does. Set `handling` for
  SIGURG and set `sig.sigurg_at_init = true`. The `ignored` bit stays as
  `initsig` already computed it.
- In `sighandler`: `if (s == SIGURG) preempt_signal();`, before the
  existing `sigsend`/`forward`.
- In `signal_disable`, treat SIGURG like `sigpipe_at_init`: don't
  restore `fwd`.
- In `signal_ignore`, for SIGURG while `sigurg_at_init`: don't install
  `SIG_IGN`. Only set `ignored` and clear `wanted`. `forward()` already
  does nothing for an ignored or default-ignored SIGURG.

- [ ] **Step 4: Run the os/signal tests**

Run: `jolt build "test:filter=GoOsSignal.*"`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/signal.cpp src/runtime/runtime.hpp src/runtime/proc.cpp tests/go_os_signal_test.cpp
git commit -m "runtime: install a SIGURG handler at startup for preemption"
```

---

### Task 2: The `nopreempt` bracket and the fields sysmon reads

No behavior change yet. This task makes it safe for a signal to take
`m->p` away at any point outside the bracket.

**Files:**
- Modify: `src/runtime/runtime.hpp` (`G`, `P`, `M`)
- Modify: `src/runtime/proc.cpp`

**Interfaces:**
- Produces: `uint32_t G::nopreempt = 0;`. It is a plain field, only
  touched by the thread running the G, and is read by the signal handler
  on that same thread.
- Produces a RAII guard in `proc.cpp`'s anonymous namespace:
  `struct nopreempt_guard { G* gp; explicit nopreempt_guard(G* gp) noexcept; ~nopreempt_guard(); };`.
  - On entry: `if (gp) gp->nopreempt++; std::atomic_signal_fence(std::memory_order_seq_cst);`.
  - On exit: the fence, then the decrement.
  - It takes `nullptr` when there is no fiber (scheduler context or a
    foreign thread) and does nothing then.
- Produces `std::atomic<uint32_t> P::schedtick`, `std::atomic<bool> P::running` and
  `std::atomic<M*> P::m`. The owner writes them with relaxed stores, and
  sysmon reads them with relaxed loads.
- Produces `pid_t M::tid`, set in `mstart` from `gettid()`.
- Produces `std::atomic<P*> M::preempt_p{nullptr}` and
  `std::atomic<uint32_t> M::preempt_tick{0}`: the request.
- Produces `std::atomic<P*> M::retaken{nullptr}`: the handler's answer.

- [ ] **Step 1: Record a baseline benchmark on unchanged `master`**

Run: `jolt build benchmark && cp report/benchmark.md /tmp/claude-1000/-home-rbrt-git-go--/18972160-4901-4f88-8cd5-68e104278f74/scratchpad/benchmark-before.md`
Expected: the report exists. Task 4 compares against it.

- [ ] **Step 2: Add the fields and the guard**

Convert the `P` fields above to atomics. Reads and writes in `proc.cpp`
use explicit relaxed `load`/`store`; `schedtick++` becomes
`store(load()+1)`, keeping a single writer and no read-modify-write.

- [ ] **Step 3: Bracket the fiber-side entry points**

Wrap the whole body of each of these in
`nopreempt_guard g{fiber_or_null}`, where `fiber_or_null` is
`m && m->curg ? m->curg : nullptr`, read right after `getm()`:

- `newproc`, `ready`, `injectglist`
- `park`, `Gosched`, `entersyscall`, `exitsyscall`, `has_runnable`

For `park`, `Gosched` and `exitsyscall`, the guard stays held across
`mcall`. The fiber may resume on another M, so after `mcall` returns the
guard's destructor must decrement the same G it captured. Capture `gp`,
not `m`.

In `gentry`, set `gp->nopreempt = 1` before `return std::move(gp->caller);`.
In `newproc`, reset `gp->nopreempt = 0` when reusing a G.

- [ ] **Step 4: Keep the `execute` invariant**

In `execute`:
- All `m->p` accesses (`schedtick`) come before `m->curg = gp`, with a
  `std::atomic_signal_fence` between them.
- After `resume()` returns, `m->curg = nullptr` is the first statement,
  followed by a fence, before any `m->p` use.

`mstart` records `m->tid = gettid()` before `acquirep`.

- [ ] **Step 5: Run the full test suite**

Run: `jolt build test`
Expected: all PASS, the same as on `master`.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/runtime.hpp src/runtime/proc.cpp
git commit -m "runtime: mark where fibers use their P, for preemption"
```

---

### Task 3: sysmon retakes the P from long-running fibers

**Files:**
- Modify: `src/runtime/proc.cpp` (`sysmon`, `preempt_signal`, `schedtrace`, `init`)
- Test: `tests/runtime_test.cpp`

**Interfaces:**
- Consumes: Task 1's `preempt_signal()` hook, and all of Task 2's fields
  and `nopreempt_guard`.
- Produces: `std::atomic<uint64_t> sysmon_retakes` and the schedtrace
  field `retake=`. Also `bool preempt_off`, read once from `GODEBUG` in
  `init`.

- [ ] **Step 1: Write the failing tests** in `tests/runtime_test.cpp`

Helper: `void spin_for(std::chrono::milliseconds d)`. It busy-loops on
`std::chrono::steady_clock` with no runtime calls.

- `TEST(Runtime, PreemptSpinnersDontStarveSleeper)`:
  - Start `GOMAXPROCS()` fibers that each `spin_for(500ms)`.
  - Then, from the test's thread, `go` a fiber that does
    `time::Sleep(10 * time::Millisecond)` and sends `time::Since(start)`
    on a channel.
  - `EXPECT_LT(elapsed, 100 * time::Millisecond)`.
  - Then wait for the spinners.
- `TEST(Runtime, PreemptSpinnersDontStarveNewFiber)`:
  - Same spinners.
  - A fiber started with `go` after them sends on `ran`; `ran.recv()`
    must arrive within 100 ms, checked with `select` against
    `time::After(100ms)`.
  - Also run a 100-round ping-pong between two new fibers during the
    spin, and `EXPECT_EQ` its result.
- `TEST(Runtime, PreemptedFiberParksAndResumes)`:
  - `GOMAXPROCS()` spinners, each doing `spin_for(100ms)`, then
    `c.recv()` on its own channel, then `spin_for(50ms)`, then
    `done.send(i)`.
  - The test sends to each channel after 150 ms, and collects every `i`
    within 2 s.
- `TEST(Runtime, PreemptUnderChurn)`:
  - `GOMAXPROCS()` spinners of 300 ms.
  - Meanwhile 8 fibers each do 20000 rounds of
    `go` + `WaitGroup`/channel handoffs and `runtime::Gosched()`, so
    signals land in `ready`/`runqput`/`execute`.
  - All complete, with the expected counts.
- `TEST(Runtime, RawBlockingSyscallLosesP)`:
  - `GOMAXPROCS()` fibers each block in raw `::poll(nullptr, 0, 300)`,
    without `blocking_call`, retrying on `EINTR` until 300 ms have
    passed.
  - A new fiber's send arrives within 100 ms.
- `TEST(Runtime, PreemptOffSwitch)`:
  - Runs itself as a child with `GODEBUG=asyncpreemptoff=1` and
    `GOMAXPROCS=1`, using the `run_child` pattern from
    `tests/go_os_pipe_test.cpp`; copy the helper, it is file-local.
  - The child part is selected by an env var `GOPP_TEST_CHILD=1`. It
    spins for 300 ms with a sleeper as in the first test, and exits 0 if
    the sleeper took ≥ 250 ms, 1 otherwise.
  - The parent `EXPECT_EQ(WEXITSTATUS(status), 0)`.

- [ ] **Step 2: Run them and check that they fail**

Run: `jolt build "test:filter=Runtime.Preempt*:Runtime.RawBlocking*"`
Expected: the starvation tests FAIL on time; `PreemptOffSwitch` may
already pass.

- [ ] **Step 3: Implement `preempt_signal()`**

The handler on the M's own thread. It reads `tls_m` directly (not
`getm()`) and declines (just clears the request) unless all of these hold:

- `m` is an M;
- `P* pp = m->preempt_p.exchange(nullptr)` is `m->p`;
- `pp->schedtick.load() == m->preempt_tick.load()`;
- `m->curg != nullptr`;
- `m->curg->nopreempt == 0`.

To take the P, set `m->p = nullptr`, `pp->m.store(nullptr)` and
`pp->running.store(false)`. Then `m->retaken.store(pp, release)`. Save
and restore `errno`.

- [ ] **Step 4: Detection and handoff in `sysmon`**

- Keep a sysmon-local array `struct { uint32_t tick; int64_t when; } pd[gomaxprocs]`.
- On each tick, unless `preempt_off`, for each `P`:
  - If `!running`, or `m` is null, or `tick` changed: record `{tick, now}`.
  - Else, if `now - when >= force_preempt` and work is waiting: set
    `m->preempt_tick` and then `m->preempt_p`, `tgkill(getpid(),
    m->tid, SIGURG)`, and reset `when = now`, so there is at most one
    signal per 10 ms per P. Work is waiting when `!runqempty(pp)`, or
    `sched.runqsize > 0 && sched.npidle == 0`.
- Then, for every M on `sched.allm`, if `P* pp = m->retaken.exchange(nullptr)`,
  call `startm(pp, false)` and count `sysmon_retakes`.
- A retake request counts as activity for sysmon's backoff: reset
  `delay` to 20 µs, so the acknowledgement is picked up on the next tick.

`sched.allm` is walked under `sched.lock`, as `schedtrace` does. Collect
the retaken Ps under the lock, then call `startm` after releasing it,
because `startm` takes `sched.lock` itself.

- [ ] **Step 5: Off switch and trace**

- `init`: `preempt_off = GODEBUG contains "asyncpreemptoff=1"`, parsed
  like `netedns0Off` in `src/net/dnsclient.cpp`.
- `schedtrace` appends `" retake=" + std::to_string(sysmon_retakes)`
  after `sysmon_timers`.

- [ ] **Step 6: Run the new tests, then everything**

Run: `jolt build "test:filter=Runtime.*"`, then `jolt build "test:repeat=20,filter=Runtime.Preempt*:Runtime.RawBlocking*"`, then `jolt build test`
Expected: all PASS, including 20 repeats with no flakes.

- [ ] **Step 7: Commit**

```bash
git add src/runtime/proc.cpp tests/runtime_test.cpp
git commit -m "runtime: retake the P from fibers that run for 10ms"
```

---

### Task 4: Benchmark check, README and the spec

**Files:**
- Modify: `README.md` (the "No preemption" bullet under Status; the runtime row of "What's in the box")
- Add: `docs/superpowers/specs/2026-11-13-preemption-design.md`, `docs/superpowers/plans/2026-11-13-preemption.md`

- [ ] **Step 1: Benchmark against the baseline**

Run: `jolt build benchmark`, then compare `report/benchmark.md` with
`/tmp/claude-1000/-home-rbrt-git-go--/18972160-4901-4f88-8cd5-68e104278f74/scratchpad/benchmark-before.md`.
Expected: go++ req/s, p99 and cycles per request within run-to-run noise
(a few %) at 1, 64 and 256 connections. If not, stop and report the
numbers.

- [ ] **Step 2: Update the README**

Replace the "No preemption" bullet with a "Preemption" bullet:
- Say that a fiber computing for over 10 ms while other fibers wait
  loses its processor to another thread, which runs the waiting fibers.
- Say that the fiber itself is never switched out or moved mid-code, so
  `errno`, `thread_local` values and locks stay valid. It gets a
  processor back when it next blocks or yields.
- Mention that system calls made without `runtime::blocking` may see
  `EINTR` from SIGURG, as in Go.
- Mention `GODEBUG=asyncpreemptoff=1` to turn it off.

In the runtime row, add "preemption of long-running fibers" after "M:N
scheduler with work stealing".

- [ ] **Step 3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-11-13-preemption-design.md docs/superpowers/plans/2026-11-13-preemption.md
git commit -m "README: preemption of long-running fibers"
```
