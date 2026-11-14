# Examples

Each example is one `main.cpp`, written as you'd write the program in Go.
The comment at the top of each file shows how to run it and what to expect.

## Build and run

```sh
jolt build examples/hello                          # one example
jolt build examples                                # all of them
./artifacts/examples_hello/bin/examples_hello      # run it
```

Jolt links each build into `artifacts/`, with dashes in the name turned
into underscores: `examples/tcp-echo` builds
`artifacts/examples_tcp_echo/bin/examples_tcp_echo`. The programs with
flags print them with `-h`.

## Where to start

If you're new to go++, read them in this order:

1. [prime](prime/main.cpp): goroutines and channels, nothing else.
2. [hello](hello/main.cpp): an HTTP server with routes and graceful shutdown.
3. [search](search/main.cpp): `select`, timeouts and context cancellation.
4. [todo](todo/main.cpp): a whole service: HTTP, JSON, SQL and contexts.
5. [panic](panic/main.cpp): what happens when one of them goes wrong.

## Concurrency

| Example | What it shows |
|---|---|
| [prime](prime/main.cpp) | The concurrent prime sieve: a pipeline of fibers, one per prime, joined by channels. `go`, `make_chan`, `range` over a channel, `sync::WaitGroup` |
| [mandelbrot](mandelbrot/main.cpp) | A worker pool: rows go out on one channel, rendered rows come back on another, and are printed in order |
| [search](search/main.cpp) | The replicated search from "Go Concurrency Patterns": each query goes to several replicas, the first answer wins, and a `context::WithTimeout` deadline bounds the search. Canceling the context stops the replicas that lost |
| [panic](panic/main.cpp) | Crashes on purpose, to show what go++ prints when a program dies: the panic, then the goroutine's stack with files, lines and `created by`. `-crash` picks the crash: `panic`, a slice index out of range, a send on a closed channel, an uncaught exception, or a nil pointer. Run it with `GOTRACEBACK=all` to see every goroutine and what it's waiting for |

## Networking

| Example | What it shows |
|---|---|
| [hello](hello/main.cpp) | An HTTP server: `ServeMux` patterns (`GET /hello/{name}`), HTTPS and HTTP/2 with `-cert`/`-key`, h2c, and graceful `Shutdown` on Ctrl-C through `signal::NotifyContext`. It's the server in the benchmark |
| [fetch](fetch/main.cpp) | A small curl: HTTP/1.1, HTTPS and HTTP/2, `-X`/`-d`/`-i`, and a load generator (`-n 1000 -c 50`). Ctrl-C cancels the requests in flight |
| [httpfileserver](httpfileserver/main.cpp) | Serves a directory with `http::FileServerFS` over an `os::Root`, so no request gets outside it. Ranges and conditional requests come for free, files go out with `sendfile(2)`, and a `ResponseWriter` wrapper logs each request |
| [todo](todo/main.cpp) | A JSON to-do API over SQLite: `database/sql` with the `sqlite3` driver, structs to and from JSON by reflection, and queries that run with their request's context |
| [proxy](proxy/main.cpp) | A TCP proxy: `net::Dialer`, `io::Copy` between sockets with `splice(2)`, and half-close with `CloseWrite` |
| [lookup](lookup/main.cpp) | Concurrent DNS lookups with go++'s port of Go's resolver: every record type of every name in a fiber of its own, under one deadline |
| [tcp-echo](tcp-echo/main.cpp) | The smallest TCP server: a fiber per connection, and `io::Copy` from the connection to itself |
| [udp-echo](udp-echo/main.cpp) | A UDP echo server with `netip::AddrPort`s, which don't allocate per packet |
| [tcp-chat](tcp-chat/main.cpp) | A chat room: one fiber owns the client list, and the connections' fibers send it join, message and leave events. Share memory by communicating |

## Files, processes and text

| Example | What it shows |
|---|---|
| [grep](grep/main.cpp) | Searches a tree: one fiber walks it with `fs::WalkDir`, a pool of fibers reads and searches the files (on io_uring where the kernel has it), and the errors are collected with `errors::Join` |
| [sha256sum](sha256sum/main.cpp) | coreutils' `sha256sum`, hashing several files at once: `crypto/sha256`, `md5`, `sha1` and `sha512` behind `hash::Hash`, `io::Copy` into a hash, `hex`, a buffered channel as a semaphore, and results printed in order. `-c` checks a list, and its output is interchangeable with coreutils' |
| [wordfreq](wordfreq/main.cpp) | Counts the words of files, a fiber per file, with Go's Unicode tables (`unicode::IsLetter` in any script), `strings::FieldsFunc`, `maps::DeleteFunc`, and `slices::SortFunc` with `cmp::Or` to sort by count, then by word |
| [parallel](parallel/main.cpp) | Runs shell commands a few at a time, like `xargs -P`: `os/exec` with `CommandContext`, an `io::Writer` of its own as each command's `Stdout`, which prefixes the lines with the job number, and timeouts and Ctrl-C that kill the processes |
| [sandbox](sandbox/main.cpp) | Runs a command in new Linux namespaces, as an ordinary user: `SysProcAttr` with clone flags and user ID mappings |
| [json-file](json-file/main.cpp) | A task list saved as JSON: `json::Marshal` by reflection, errors wrapped with `fmt::W` (Go's `%w`) and tested with `errors::Is`, and atomic saves with `os::CreateTemp` and `os::Rename` |
| [template](template/main.cpp) | Renders a Markdown report from plain C++ structs with `text/template`, reading their fields by reflection, with a C++ lambda as a template function |

## Using go++

| Example | What it shows |
|---|---|
| [modules](modules/main.cpp) | go++ imported as C++ modules (`import gopp.encoding.json;`) instead of included as headers |

## Adding an example

An example is a directory with a `main.cpp` and a task in
[build.jolt](../build.jolt):

```python
class MyExample(Example):
    """ One line on what it shows. """
    name = "examples/my-example"
    sources = ["examples/my-example/*.cpp"]
```

Add its name to the `examples` alias at the end of the list, so that
`jolt build examples` builds it too, and add it to the tables above.
