# Goroutine tracebacks on crashes

## Intent

When a go++ program dies, it should say where, as a Go program does: the
panic message, then the stacks of the goroutines, each with file, line and
the goroutine that created it. Today `panic` prints the message and the
goroutine id, and a segfault or an uncaught exception leaves nothing but a
core.

## Goals

- `gopp::panic`, an exception that escapes a fiber, `runtime::fatal`, and
  the signals SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT, SIGSYS and
  SIGQUIT print Go's report: `panic: msg` (or `fatal error: msg`, or
  `SIGQUIT: quit` with the PC), `[signal SIGSEGV: ...]` for faults, then
  `goroutine N [status]:` blocks of `function(...)` and `\tfile:line +0xoff`
  lines, ending with `created by F in goroutine M`.
- Wait reasons in the status, as in Go: `chan receive`, `chan send`,
  `select`, `select (no cases)`, `sleep`, `IO wait`, `semacquire`,
  `sync.Mutex.Lock`, `sync.RWMutex.RLock`, `sync.RWMutex.Lock`,
  `sync.WaitGroup.Wait`, `sync.Cond.Wait`.
- `GOTRACEBACK` as in Go: `none` (0), `single` (default), `all` (1),
  `system` (2), `crash`. Panics print the crashing goroutine; fatal errors
  and fatal signals print all of them. `system` shows runtime frames.
- After the report the process exits with status 2; with
  `GOTRACEBACK=crash` it dies of SIGABRT and dumps core.
- An exception that escapes a fiber is reported from where it was thrown,
  with the frames between there and the fiber's start, before any
  destructor runs.
- Stack overflow on a fiber (its guard page) is reported as Go's
  `fatal error: stack overflow`.
- Threads the runtime didn't start that use it (the main thread, for
  one) are listed too, as `thread TID [status]:`.

## Non-goals

- `recover`. A panic still ends the program.
- Function arguments. Go prints argument words; here frames print as
  `name(...)`.
- Wait durations (`, 5 minutes`) and `locked to thread`.
- Deadlock detection (`all goroutines are asleep`).

## Design

**Symbolization** uses the libbacktrace that GCC bundles in
`libstdc++exp.a` (`__glibcxx_backtrace_*`): DWARF file and line,
inlined frames, and symbol names from the symbol table for code without
debug information. Names are demangled with
`__gcclibcxx_demangle_callback`, which doesn't allocate. libbacktrace
allocates with mmap, so the report is close to async-signal-safe. The
state is created at the first crash. The user picked this over a new
libbacktrace package in jolt; the names are libstdc++ internals that GCC 16
has, and go++ needs GCC 16.

**Stacks.**
- The crashing thread unwinds itself with `_Unwind_Backtrace` (DWARF
  CFI), which also walks through libstdc++, libc and the signal
  trampoline. In a signal handler it skips up to the interrupted PC; in
  `panic` and `fatal`, up to the caller's frame.
- A parked or runnable fiber is walked by frame pointers (go++ builds with
  `-fno-omit-frame-pointer`). `mcall` records its frame address in the G
  when the fiber switches out, and `newproc` records the stack's bounds. A
  walk stops at a frame outside the stack, or at `gentry`. Stacks are never
  unmapped, so a walk racing with the fiber's resumption reads garbage at
  worst, never faults.
- A fiber running on another M, and a foreign thread, are asked to dump
  themselves: the dumping thread sends SIGURG with a request naming the
  thread, and the handler prints its own stack from its context, then
  blocks until the process exits. A thread that doesn't answer within
  500 ms is reported as `stack unavailable`.
- The G records the return address of `newproc`'s caller and the creating
  goroutine's id for `created by`. `gopp::go` is always inlined, so that
  address is in the user's function.
- All Gs are on a lock-free list (`allgs`), pseudo-Gs of foreign threads
  included; a thread that exits marks its pseudo-G dead.

**Exceptions.** `gentry` calls the fiber's function through a frame
whose unwind info names a personality routine of go++'s own
(`.cfi_personality`). In the search phase, which runs before any
unwinding, the routine finds no handler between the throw and the fiber's
start, and reports the panic there, with the stack intact. A
`noexcept` wrapper would not do: GCC unwinds through cleanups to the
noexcept frame before calling `std::terminate`. On other threads, an
uncaught exception reaches `std::terminate` from `__cxa_throw` without
unwinding, and go++'s terminate handler reports it the same way.

**Hidden frames** (all but `system`): `gopp::runtime` and
`boost::context` functions, the invoke plumbing of libstdc++ (frames in
`bits/invoke.h`, `bits/funcwrap.h`, `bits/mofunc_impl.h`,
`bits/std_function.h`, `bits/std_thread.h`, `<tuple>`), `gopp::go`'s
lambda, the libc start and thread start frames, and the frames of the
throw machinery above the throwing function.

**Signals.** The crash signals get the runtime's handler at startup if
their disposition is the default one, with `SA_ONSTACK` and a 64 KiB
alternate stack on every M and on threads that use the runtime. SIGQUIT
and SIGABRT can still be taken by `signal::Notify`. A second crash on
another thread waits (answering dump requests) while the first reports; a
crash during the report ends the process at once.

## Testing

Death tests that run small crashing programs (panic in a fiber, a
segfault, an exception thrown under a destructor, a fatal error with
fibers blocked on a channel, a mutex and a sleep) and match the report:
the header, wait reasons, the user's frames with file and line, and
`created by` lines; and that runtime frames are hidden.
