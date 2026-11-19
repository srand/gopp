# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

go++ is Go's standard library (ported from the Go 1.27.3 sources) as a C++26 framework, on a Go-style runtime: stackful fibers (Boost.Context) scheduled M:N with work stealing, an epoll netpoller, and io_uring for file system calls. It keeps Go's names and semantics. Linux x86-64 only for now. It needs GCC 16 for static reflection (`-freflection`, P2996), which `encoding/json`, `encoding/binary`, `text/template` and `slog` rely on. Clang won't work.

## Build and test

Everything builds with [Jolt](https://github.com/srand/jolt) through `build.jolt`. Jolt fetches and builds the toolchain and dependencies (GCC 16, Boost.Context, liburing, OpenSSL, zlib, SQLite, libpq, googletest), so the host compiler doesn't matter. Compiling uses `-Wall -Wextra -pedantic -Werror`, so a warning fails the build.

```sh
jolt build gopp                          # the library (libgopp.a + include/gopp/)
jolt build test                          # build and run all unit tests
jolt build test:filter='Json*'           # run a subset (googletest filter)
jolt build test:leaks=true,repeat=2      # fail tests that leak gopp::objects (round 1 is a warm-up)
jolt build coverage                      # lcov report (unoptimized, -fprofile-update=atomic)
jolt build examples/hello                # one example; `examples` builds them all
jolt build benchmark                     # go++ vs Go hello server; report/benchmark.md
```

Outputs go to `artifacts/<task_name>/bin/` (`/` in a task name becomes `_`). After `jolt build gopp/tests` you can run the test binary directly, which is faster when you iterate: `artifacts/gopp_tests/bin/gopp_tests --gtest_filter='Suite.Name'`.

Other libraries are separate Jolt tasks: `gopp/sqlite3`, `gopp/postgres`, `gopp/x/jose`, `gopp/x/oauth2`, `gopp/x/oidc`. `gopp/tests` links all of them.

After you add a public header or change a header's `#include`s, regenerate the module interfaces with `python3 modules/generate.py` from the repo root.

## Layout and architecture

- `include/gopp/` holds the public headers, one per Go package (`<gopp/net/http.hpp>` is `net/http`). Namespaces follow the package path's last element under `gopp` (`gopp::http`, `gopp::json`). Two exceptions: `text/template` is `gopp::text::tmpl` and `html/template` is `gopp::html::tmpl`, because `template` is a keyword.
- `src/<package>/` holds the implementations, plus internal headers (for example `src/net/http/internal.hpp`, `src/runtime/runtime.hpp`). The library's include path has both `include` and `src`.
- `src/runtime/` is the core everything else parks on:
  - `proc.cpp`: the scheduler, after Go's `proc.go` (Ms, Ps, Gs, work stealing, sysmon, preemption)
  - `chan.cpp`: channels and `select`
  - `netpoll.cpp`: epoll, which also reaps the io_uring rings
  - `uring.cpp`: per-thread io_uring for file calls
  - `timer.cpp`, `sema.cpp`, `stack.cpp`, `signal.cpp`
  - `traceback.cpp`: Go-style goroutine dumps on crashes
- `drivers/` (`sqlite3`, `postgres`) and `x/` (`jose`, `oauth2`, `oidc`) are separate libraries, each with its own `include/` and `src/`. A new one needs:
  - its own task in `build.jolt`, with a `publish` that collects its headers
  - an entry in the `Tests` task's `requires`, `incpaths` and `sources`
  - for `x/`, a row in `x/README.md`
- `modules/` holds the generated C++ module interfaces (`gopp.net.http`, …), one per header. Each public header must keep its `#include`s in one block wrapped in `#ifndef GOPP_MODULE` … `#endif`. The generator enforces this.
- `tests/` holds the googletest tests. The test binary has `src` and the drivers' and `x/`'s `src` on its include path, so tests can use internal headers.
- `benchmarks/` holds the Go reference server and the load generators (Go and go++), plus `files-gopp`.
- `docs/superpowers/specs/` holds the design records for larger features. Update the relevant spec when a decision changes.

### Runtime model to keep in mind

- Anything that waits must **park the fiber**, not block the thread. Channels, `sync`, timers, `context`, network I/O and io_uring file calls already park. A blocking syscall that can't park must go through `runtime::blocking`, which hands the P to another thread, as Go's entersyscall does.
- **Never park inside a function-local `static` initializer or `std::call_once`.** That includes os file I/O, `sync::Mutex`, channels and `runtime::blocking`. Other fibers block their threads, and keep their Ps, on the C++ guard, which deadlocks. Let the static only allocate, and do the loading under `sync::Once`, as `src/mime/type.cpp` and `src/net/dnsconfig.cpp` do. Plain raw syscalls are fine. The `Net.ConcurrentFirstUse` test covers this case.
- Preemption moves the P, not the fiber: a fiber is never switched out mid-code, so `thread_local` and `errno` stay valid. Syscalls made outside `runtime::blocking` may get `EINTR` from SIGURG.
- Sockets and pipes stay on epoll on purpose. io_uring for networking was benchmarked and lost.

## Conventions

- **Exported names use Go's exact spelling**, methods and struct fields included: `http::Get`, `w.Header().Set(...)`, `resp->StatusCode`, `http::Server{.Addr = ":8080"}`. Go builtins stay lowercase: `error`, `string`, `go`, `select`, `make_chan`, and a channel's `send`, `recv` and `close`. C++ protocol members stay too (`lock`/`unlock` sit beside `Lock`/`Unlock`).
- **Private names follow Go's unexported style.** Functions, variables, parameters and data members are mixedCaps (`trySend`, `leftDelim_`). Types are lowerCamel (`loggingWriter`, `pollDesc`). Private data members end in an underscore. Some names stay snake_case: `make_chan`, `make_ptr`, `intrusive_ptr`, `as_bytes`, the `*_pointer_cast`s, namespaces (`go_helper`, `os_test`), and C symbols referenced from asm. Check a neighbouring header before you pick a name.
- **Errors are values.** Functions return `gopp::error`, which is nil on success. Multiple results come back as `std::pair`/tuple for structured bindings (`auto [n, err] = ...`), not `std::expected`.
- **Shared objects derive from `gopp::object` and are held by `gopp::ptr<T>`** (an intrusive refcount). Don't use `shared_ptr`. `gopp::string` and `gopp::slice<T>` share their buffers when copied or sliced, as Go's do.
- **Interfaces are abstract classes.** Type assertions are `dynamic_cast`.
- **Names that collide with C macros take a trailing underscore**: `io::EOF_`, `os::O_RDONLY_`, `syscall::SIGTERM_`, `syscall::ECHILD_`.
- Doc comments name the Go symbol they port (`// sync.Mutex, with Go's normal and starvation modes.`) and say where go++ differs from Go.
- Don't reintroduce C++ coroutines, `shared_ptr`, or non-Linux backends.

## Tests

- `tests/go_*_test.cpp` are ports of Go's own tests, and upstream's for `x/` and the drivers. Keep Go's test names and tables. Start the file with a comment that names the Go source file and version it was ported from, and what was left out and why.
- Tests that need a child process run the test binary again as a helper command (`tests/go_helper.hpp`, `helperCommand("name", args)`), as Go's `os/exec` tests do.
- When you add a package or ported tests, update `README.md`: the package table, the "Tested against Go's own tests" paragraph, and the test count.

## Debugging knobs

- `GOTRACEBACK=none|single|all|system|crash`: what a crash prints, as in Go.
- `GOPP_SCHEDTRACE=<ms>`: print the scheduler's counters every `<ms>` milliseconds.
- `GOPP_STACK_SIZE`: the fiber stack size (256 KiB by default).
- `GOPP_IOURING=0`: make file calls plain blocking calls instead of io_uring.
- `GODEBUG=asyncpreemptoff=1`: turn preemption off.
- `GOMAXPROCS`: the number of Ps, as in Go.
- Builds keep frame pointers (`-fno-omit-frame-pointer`, `-g`), so `perf record --call-graph fp` works.
- When you kill a running example, use `pkill -x <name>`, not `pkill -f`: the Bash tool's own command line would match.

## Git

- Subjects start with the package path: `net/http: trailers`, `x/oidc: ...`, `build: ...`, `docs: ...`.
- Keep history linear: rebase branches (including agent worktree branches) onto master. Never create merge commits. Intermediate commits don't need to build. Testing the final tree is enough.
- `examples/fstree` is a benchmark that lives on a branch. Never commit it to master.
