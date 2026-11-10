# go++

**Write C++ like Go. Run it faster than Go.**

go++ is Go's standard library as a C++26 framework: `net/http`, `encoding/json`,
`context`, `sync`, goroutines, channels and `select`. It keeps Go's names
and Go's semantics, ported from the Go 1.27.3 sources. Underneath is a Go-style
runtime: stackful fibers scheduled M:N over OS threads with work stealing,
and an epoll network poller. Handlers are plain blocking code, with no
callbacks, no `co_await` and no function coloring. You get native code with
no garbage collector.

```cpp
#include <gopp/encoding/json.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net/http.hpp>

using namespace gopp;

struct Item {
    string id;
    string name;
    [[=json::tag("price_cents,omitempty")]] int64 price;   // a Go struct tag
};

int main() {
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /items/{id}", [](http::ResponseWriter& w, http::Request& r) {
        auto [item, ok] = db.Lookup(r.PathValue("id"));
        if (!ok) {
            http::Error(w, "no such item", http::StatusNotFound);
            return;
        }
        w.Header().Set("Content-Type", "application/json");
        json::NewEncoder(w).Encode(item);        // struct to JSON by C++26 reflection
    });
    http::Server srv{.Addr = ":8080", .Handler = mux};
    if (auto err = srv.ListenAndServe()) fmt::Println("{}", err);
}
```

If you read Go, you can already read this. Every request runs on its own
fiber, as each request runs on its own goroutine in Go.

## Faster than Go, on Go's home turf

The same hello-world server, written once with Go's `net/http` and once with
go++, under the same load on the same machine (`jolt build benchmark`):

| Load | Throughput | p99 latency | CPU cycles per request | Memory |
|---|---:|---:|---:|---:|
| 1 connection | **1.3x** Go | **0.65x** Go | **0.56x** Go | 4.7 vs 13.9 MiB |
| 64 connections | **1.9x** Go | **0.34x** Go | **0.50x** Go | 5.9 vs 15.2 MiB |
| 256 connections | **1.5x** Go | **0.34x** Go | **0.50x** Go | 9.9 vs 19.5 MiB |

With a path parameter (`/hello/{name}`), go++ serves 1.3 to 1.6 times Go's
requests per second, at 0.27 to 0.70 times Go's p99 latency.

go++ serves the same requests with half the CPU cycles, a third of Go's p99
latency under load, and a third of the memory. The benchmark pins the server
and the load generator to separate cores. It also measures CPU cycles per
request, which stay comparable when a laptop throttles. The full table is in
`report/benchmark.md` after a run.

## Concurrency the Go way

```cpp
// Fetch URLs concurrently; print each result as it arrives, or give up.
auto results = make_chan<string>(urls.size());
for (const string& u : urls) {
    go([u, results] {
        auto [resp, err] = http::Get(u);
        if (err) return results.send(err.Error());
        resp->Body->Close();
        results.send(u + ": " + resp->Status);
    });
}
for (std::size_t i = 0; i < urls.size(); i++) {
    select(recv(results, [](string line) { fmt::Println("{}", line); }),
           recv(time::After(2 * time::Second), [] { fmt::Println("timed out"); }));
}
```

Fibers are cheap: a 256 KiB stack of virtual memory, of which only the touched
pages are real. A blocked fiber parks and frees its thread for others.
Channels, `select`, mutexes, timers, `context` cancellation and network I/O
all park instead of blocking the thread.

## What's in the box

| Package | What you get |
|---|---|
| runtime | `go()`, M:N scheduler with work stealing (Go's `proc.go` design), epoll netpoller, timers, semaphores |
| `net/http` | Server with Go 1.22 `ServeMux` patterns (`GET /items/{id}`, `{path...}`, `{$}`, conflict detection), keep-alive, pipelining, chunked encoding, content sniffing, timeouts, graceful `Shutdown`, HTTPS. Client with `http::Get`/`Head`/`Post`, `Client` with redirects and timeouts, pooling `Transport`, HTTPS |
| `net/http/httptest` | `NewRecorder`, `NewRequest`, `NewServer`, `NewTLSServer` |
| `crypto/tls` | TLS 1.2/1.3 over any `net::Conn` (OpenSSL underneath), ALPN, SNI, certificate verification, `tls::Listen`/`Dial` |
| `encoding/json` | `Marshal`/`Unmarshal` of plain structs through C++26 reflection, with no macros and no registration. Struct tags as annotations (`omitempty`, `string`, `-`), `Encoder`/`Decoder` streams, `json::Value` for Go's `any`, Go's error messages and number formatting |
| `net`, `net/url` | TCP with deadlines, URL parsing and escaping |
| `context`, `sync`, `time` | Cancellation and deadlines; `Mutex`, `RWMutex`, `WaitGroup`, `Once`, `Cond`; `Timer`, `Ticker`, `AfterFunc` |
| `io`, `bufio`, `fmt`, `strings`, `strconv`, `errors`, `flag`, `os` | The everyday packages, with `fmt` on `std::format` |

**Tested against Go's own tests.** 191 unit tests, 86 of them ported
from Go: `net/http`'s routing and ServeMux tests, `encoding/json`'s scanner,
encoding and decoding tests, and HPACK's RFC 7541 vectors. They keep Go's
test names and tables.

## Get started

go++ builds with [Jolt](https://github.com/srand/jolt), which builds every
dependency on first use: GCC 16, Boost.Context, liburing, OpenSSL and
googletest. Your host compiler doesn't matter.

```sh
jolt build examples/hello        # the hello-world server
jolt build test                  # run the unit tests
jolt build benchmark             # go++ vs Go, report in report/benchmark.md
```

| Task | What it builds |
|---|---|
| `jolt build gopp` | the library: `libgopp.a` and `include/gopp/` |
| `jolt build examples` | the example programs |
| `jolt build gopp/tests` | the unit test program |
| `jolt build test` | runs the unit tests |
| `jolt build benchmark` | runs `examples/hello` and the same server in Go 1.27 (`benchmarks/hello-go`) under the same load; publishes `report/benchmark.md` and `.json` |

Requirements: Linux, x86-64. go++ is C++26 with static reflection (P2996),
which GCC 16 implements (`-freflection`); Clang doesn't yet. Programs link
libstdc++ statically.

## Coming from Go

- **Packages are namespaces** under `gopp`: `http::Get`, `json::Marshal`,
  `time::Sleep`. Exported names keep Go's spelling, and that includes methods
  and struct fields: `w.Header().Set(...)`, `r.PathValue("id")`,
  `resp->StatusCode`, `http::Server{.Addr = ":8080", .Handler = mux}`.
- **Builtins stay lowercase**: `error`, `string`, `go`, `select`,
  `make_chan`, `panic`, and channels' `send`, `recv` and `close`.
- **Errors are values.** Functions return `gopp::error`, which is nil on
  success, and multiple results come as pairs:
  `auto [n, err] = conn->Read(buf);`.
- **Pointers are reference-counted.** There's no garbage collector: values
  and RAII by default. Shared objects derive from `gopp::object` and are held
  by `gopp::ptr<T>`, an intrusive count. Copying or slicing a `gopp::string`
  or `gopp::slice<T>` doesn't copy bytes.
- **Interfaces are abstract classes**, and type assertions are
  `dynamic_cast`: `if (auto* f = dynamic_cast<http::Flusher*>(&w)) f->Flush();`.
- **`fmt` uses `std::format`'s `{}`** instead of Go's verbs.
- **Threads the runtime didn't start can join in.** The main thread, for
  example, can use channels, locks, timers and the network; it blocks the
  thread where a fiber would park.
- Two spellings differ from Go. `io.EOF` is `io::EOF_`, because `EOF` is a C
  macro. And `url::Error` can't declare `Error()`, because C++ reserves a
  class's own name for its constructors: call it through `gopp::error`, or
  use `message()`.

## Under the hood

```
include/gopp/       public headers, one per Go package
src/runtime/        scheduler (proc.cpp), channels and select (chan.cpp),
                    netpoller (netpoll.cpp), timers, semaphores, stacks
src/<package>/      the other packages, ported from Go 1.27.3
examples/           example programs
benchmarks/         the Go and go++ servers and load generators
tests/              googletest unit tests; tests/go_*_test.cpp are Go's
```

`GOPP_SCHEDTRACE=<ms>` prints the scheduler's counters to standard error
every `<ms>` milliseconds, like Go's `GODEBUG=schedtrace`. It shows fibers
scheduled, steals, network polls, threads parked and woken, and I/O that
would block. `GOPP_STACK_SIZE` sets the fiber stack size.

The benchmark's load generator exists in Go (`benchmarks/load-go`, the
default) and in go++ (`benchmarks/load-gopp`, `benchmark:loader=gopp`).
Both send `http.Get` requests in a loop over keep-alive connections.
Options:
`benchmark:connections=1+64+256,paths=/+/hello/gopher,duration=5,warmup=1,rounds=2,cooldown=0,loader=go,server_cpus=6-9,client_cpus=0-5`.
By default the load generator gets the fastest cores. The servers take
turns, and the report records each side's clock.

## Status

go++ is a prototype, and a fast one. It is in active development; here is
what it doesn't do yet:

- **HTTP/2** is in progress. HPACK and the framer are done and tested, and
  the server is written. The client transport is next, and then h2 is
  switched on by default over TLS and as h2c with prior knowledge.
- **No preemption.** A fiber that computes without blocking keeps its
  thread until it blocks or calls `runtime::Gosched()`.
- **Fixed-size fiber stacks**, set by `GOPP_STACK_SIZE` (256 KiB of
  virtual memory by default).
- **One global timer heap**, where Go keeps one per P.
- **epoll only**; io_uring is next.
- **x86-64 only for now.** The code supports ARM64, but it isn't built or
  tested there yet.
- **`flag` has only the Var forms** (`StringVar`, `IntVar`, ...).
- **Request contexts** are canceled when the handler returns or a write to
  the client fails, not when the client disconnects while the handler is
  idle.
