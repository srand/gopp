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
| runtime | `go()`, M:N scheduler with work stealing (Go's `proc.go` design), preemption of long-running fibers, epoll netpoller, timers, semaphores |
| `net/http` | Server with Go 1.22 `ServeMux` patterns (`GET /items/{id}`, `{path...}`, `{$}`, conflict detection), keep-alive, pipelining, chunked encoding, content sniffing, timeouts, graceful `Shutdown`, HTTPS. HTTP/2 on both sides, negotiated over TLS or spoken as h2c (`http::Protocols`), with flow control, multiplexing and GOAWAY; tested against Go's client and server and curl. Client with `http::Get`/`Head`/`Post`, `Client` with redirects and timeouts, pooling `Transport`, HTTPS. File servers: `FileServer`/`FileServerFS` over `Dir`, `FS` or an `os::Root`, with index pages, directory listings and canonical-path redirects; `ServeFile`/`ServeFileFS`; `ServeContent` with ranges (`multipart/byteranges` for several) and the conditional requests (ETags, `If-Modified-Since`, `If-Range`, ...); the `ResponseWriter` is an `io::ReaderFrom`, as Go's is, so files go out with `sendfile(2)`, from an `FS` too, where Go copies them; `ParseTime`. Forms and uploads: `ParseForm`, `ParseMultipartForm`, `FormValue`/`PostFormValue`, `FormFile` and `MultipartReader`, with `Form`, `PostForm` and `MultipartForm` (whose temporary files the server removes after the handler); `MaxBytesReader`; `PostForm` on the client. The `Transport` asks for gzip and decompresses it transparently, as Go's does (`DisableCompression`, `Response::Uncompressed`) |
| `net/http/httputil` | `ReverseProxy` with `Rewrite` (`ProxyRequest::SetURL`, `SetXForwarded`) or `Director`, `NewSingleHostReverseProxy`, hop-by-hop header removal, `X-Forwarded-For`, query parameter smuggling protection, `ModifyResponse`, `ErrorHandler`, `FlushInterval` (streaming and server-sent events flushed as they come) and `BufferPool` |
| `net/http/httptest` | `NewRecorder`, `NewRequest`, `NewServer`, `NewTLSServer`, `NewUnstartedServer` |
| `crypto/tls` | TLS 1.2/1.3 over any `net::Conn` (OpenSSL underneath), ALPN, SNI, certificate verification, `tls::Listen`/`Dial` |
| `crypto/sha256`, `sha512`, `sha1`, `md5`, `hmac`, `hash` | Go's digests on OpenSSL's EVP: `sha256::Sum256(data)` as a `std::array<byte, 32>`, and `New()` for a running `hash::Hash` whose `Sum` doesn't stop it; SHA-224/256, SHA-384/512 and SHA-512/224 and /256, SHA-1 and MD5. `hmac::New(sha256::New, key)` over any `hash::Hash`, and `hmac::Equal` |
| `crypto/rand`, `crypto/subtle` | Random bytes from `getrandom(2)`: `rand::Read` (which never fails, as in Go 1.24) and `rand::Reader`, and `rand::Text` for 128-bit tokens. Constant-time comparisons: `ConstantTimeCompare`, `ConstantTimeSelect`, `ConstantTimeEq`, `ConstantTimeCopy`, `ConstantTimeLessOrEq` and `XORBytes` |
| `compress/gzip`, `compress/zlib`, `compress/flate` | Go's gzip and zlib formats, ported, over DEFLATE from zlib: `gzip::NewReader` with the `Header` (name, comment, modification time, extra data) and multistream files, `gzip::NewWriter`/`NewWriterLevel` with `Flush`; `zlib::NewReaderDict`/`NewWriterLevelDict` with preset dictionaries; `flate::NewReader`/`NewWriter` with Go's levels and `HuffmanOnly`, and `Resetter`. Readers read no further than their stream: from a `bufio::Reader`, or seeking a source back |
| `hash/crc32`, `hash/adler32` | `crc32::ChecksumIEEE` (zlib's fast CRC), `MakeTable` with `Castagnoli` (slicing-by-8) and `Koopman`, `New`/`NewIEEE` as `hash::Hash32`; `adler32::Checksum` and `New` |
| `encoding/hex`, `encoding/base64` | Go's hex: `EncodeToString`/`DecodeString`, `Encode`/`Decode`, the append forms, `Dump` and `Dumper` (like `hexdump -C`), streaming `NewEncoder`/`NewDecoder`. Go's base64: `StdEncoding`, `URLEncoding`, `RawStdEncoding` and `RawURLEncoding` as `constexpr` values, `NewEncoding`, `WithPadding`, `Strict`, and streaming encoders and decoders |
| `encoding/json` | `Marshal`/`Unmarshal` of plain structs through C++26 reflection, with no macros and no registration. Struct tags as annotations (`omitempty`, `string`, `-`), `Encoder`/`Decoder` streams, `json::Value` for Go's `any`, Go's error messages and number formatting |
| `database/sql` | Go's connection pool (`SetMaxOpenConns`, idle limits, lifetimes, waiting with context cancellation, bad-connection retries), `Query`/`QueryRow`/`Exec`, prepared `Stmt`s that follow connections, `Tx` rolled back when its context ends, `Conn`, `Rows`/`Scan` with Go's conversions, `Null*` types and `sql::Named`, and the `driver` interfaces |
| `sqlite3` | A SQLite driver for `database/sql` after `mattn/go-sqlite3` (`<gopp/sqlite3.hpp>`, its own library `gopp/sqlite3`): `sql::Open("sqlite3", "app.db?_foreign_keys=1&_journal_mode=WAL")` with mattn's DSN options (`_busy_timeout`, `_txlock`, `_loc`, `_synchronous`, ... and `file:` URIs), multi-statement `Exec`, `?`, `$1`, `:name`/`@name`/`$name` parameters, mattn's type conversions (`DATETIME` columns as `time::Time`, `BOOLEAN` as `bool`), `sqlite3::Error` with result codes (`ErrConstraintUnique`, ...), and contexts that interrupt queries. SQLite calls run as blocking calls, so a slow query doesn't hold up other fibers |
| `text/template` | Go's templates (`gopp::text::tmpl`, as `template` is a keyword): `{{if}}`, `{{range}}` over slices, maps in key order and integers, `{{with}}`, `{{define}}`/`{{template}}`/`{{block}}`, pipelines, variables, the builtins with Go's `printf` verbs, and plain C++ lambdas as functions. The data is any C++ struct, read by reflection: fields, const methods with arguments, containers and smart pointers |
| `net`, `net/url` | TCP, UDP, raw IP and Unix domain sockets with deadlines. `Dialer` with contexts, timeouts, Happy Eyeballs (RFC 6555) and `Control` hooks; `ListenConfig`; `Listen`/`Dial`, `ListenTCP`/`DialTCP`, `ListenUDP`/`DialUDP`, `ListenIP`/`DialIP`, `ListenUnix`/`DialUnix`; `TCPConn` with keep-alive settings (`KeepAliveConfig`), `SetLinger`, Multipath TCP, and `io::Copy` through `splice(2)` and `sendfile(2)`; `UDPConn` with `ReadFrom`/`WriteTo`, the `ReadMsg`/`WriteMsg` and `AddrPort` variants, and `ListenMulticastUDP`; `UnixConn` and `UnixListener` for `"unix"`, `"unixgram"` and `"unixpacket"`, with abstract names, autobind and descriptor passing (`syscall::UnixRights`); `SyscallConn`, `Buffers` (`writev(2)`), `Pipe`, `FileConn`/`FileListener`/`FilePacketConn`. Go's own resolver, from fibers on the poller: `Resolver` with `LookupHost`/`LookupIP`/`LookupNetIP`/`LookupCNAME`/`LookupMX`/`LookupNS`/`LookupSRV`/`LookupTXT`/`LookupAddr`, `/etc/hosts`, `resolv.conf` and `nsswitch.conf`, EDNS(0), UDP with TCP fallback, and RFC 6724 address sorting. `Interfaces`, `InterfaceAddrs`, `HardwareAddr`/`ParseMAC`; addresses as `net::Addr` (`TCPAddr`, `UDPAddr`, `IPAddr`, `UnixAddr`), `IP`/`IPMask`/`IPNet` with `ParseIP` and `ParseCIDR`, and Go's errors (`OpError`, `AddrError`, `DNSError`). URL parsing and escaping |
| `net/netip` | `Addr`, `AddrPort` and `Prefix` as small comparable values: parsing, formatting, `Compare`, masking, `Contains`/`Overlaps`, text and binary marshaling |
| `mime/multipart`, `mime/quotedprintable` | Multipart bodies, as forms post them: `multipart::Reader` with `NextPart`/`NextRawPart` and `ReadForm` (files past `maxMemory` go to one temporary file; Go's limits on parts and headers), `Part` with `FormName`/`FileName`, `Form` and `FileHeader::Open`; `multipart::Writer` with `CreateFormFile`, `WriteField`, `CreatePart` and `FormDataContentType`. Quoted-printable `Reader` and `Writer`, which parts decode by themselves |
| `net/textproto` | `MIMEHeader` (a sorted map, as Go's map is), `Reader` with `ReadMIMEHeader`, continued lines, code lines and multi-line responses (RFC 959) and dot-encoding; `Writer` with `PrintfLine` and `DotWriter`; `Pipeline`, `Conn` and `Dial` for SMTP-style clients |
| `mime` | `TypeByExtension` from Go's built-in types and the system's (the shared MIME-info database, or `mime.types`), `ExtensionsByType`, `AddExtensionType`; `ParseMediaType` with RFC 2231 continuations and encodings, and `FormatMediaType` |
| `container/heap`, `container/list` | `heap::Init`/`Push`/`Pop`/`Remove`/`Fix` over a `heap::Interface<T>` (a `sort::Interface` with typed `Push` and `Pop`); `list::List<T>`, Go's doubly linked list, whose reference-counted elements stay valid after removal, as Go's do |
| `context`, `sync`, `sync/atomic` | Cancellation and deadlines; `Mutex`, `RWMutex`, `WaitGroup`, `Once`, `Cond`; Go's atomic types (`atomic::Int64`, `Uint32`, `Bool`, ... with `Load`, `Store`, `Add`, `Swap`, `CompareAndSwap`, `And`, `Or`), `Pointer<T>` for a `ptr<T>`, a typed `Value<T>`, and the functions on plain integers (`AddInt64`, `LoadUint32`, ...) |
| `time` | Go's `Time`: zero time in year 1, wall and monotonic readings, time zones (`Location`, `UTC`, `Local` from `TZ` or `/etc/localtime`, `LoadLocation` from the system's zoneinfo or `$ZONEINFO`, `FixedZone`, `LoadLocationFromTZData`, POSIX TZ rules past the last transition), the calendar (`Date`, `Year`/`Month`/`Day`/`Clock`/`YearDay`/`ISOWeek`/`Weekday`, `AddDate`, `Truncate`/`Round`), `Format`/`Parse`/`ParseInLocation` with Go's layouts (`RFC3339`, `DateTime`, `Kitchen`, ...), `MarshalJSON`/`MarshalText`; `Duration` with `String`, `Round` and `ParseDuration`; `Timer`, `Ticker`, `AfterFunc` |
| `log`, `log/slog` | `log`: Go's logger with `std::format` messages: `Print`/`Println`/`Fatal`/`Panic`, the flags (`Ldate`, `Lmicroseconds`, `Lshortfile`, `LUTC`, `Lmsgprefix`, ...), prefixes, `Output`, and the file and line from `std::source_location`. `log/slog`: structured logging, `slog::Info("msg", "k", v, slog::Int("n", 1))`, with keys and values sorted out at compile time; `Logger` with `With`, `WithGroup` and the `Context` variants; `TextHandler` and `JSONHandler` with `HandlerOptions` (`AddSource`, `Level`, `ReplaceAttr`), `MultiHandler` and `DiscardHandler`; `Value`s of every `Kind`, `LogValuer`s, `LevelVar`; other values written with their `MarshalText` or `MarshalJSON`, `std::format`, or by reflection. The default logger writes through `log`, and `SetDefault` sends `log` through slog, as in Go |
| `slices`, `maps`, `sort`, `cmp` | Go's pattern-defeating quicksort and stable SymMerge, so sorts compare and order as Go's do: `slices::Sort`/`SortFunc`/`SortStableFunc`, `BinarySearch`, `Index`/`Contains`/`Equal`/`Compare`/`Min`/`Max`, and `Insert`/`Delete`/`Compact`/`Clone`/`Concat`/... over `gopp::slice`, `std::vector` or any contiguous range; Go's iterators as C++ ranges (`slices::Sorted(maps::Keys(m))`, `All`, `Backward`, `Chunk`, `Collect`); `maps::Clone`/`Copy`/`DeleteFunc`/`Equal`; `sort::Sort`/`Stable`/`Slice`/`Search`/`Find` over `sort::Interface`; `cmp::Compare`/`Less`/`Or` |
| `os`, `io/fs` | Files, on io_uring when the kernel has it: `Open`/`Create`/`OpenFile`, `ReadFile`/`WriteFile`, `File` with `Read`/`Write`/`ReadAt`/`WriteAt`/`Seek`, `Stat`, `Truncate`, `Sync`, `ReadDir`/`Readdir`/`Readdirnames`; paths: `Stat`/`Lstat`, `Mkdir`/`MkdirAll`, `Remove`/`RemoveAll`, `Rename`, `Chmod`/`Chown`/`Chtimes`, links, `CreateTemp`/`MkdirTemp`, `Getwd`/`Chdir`; `Pipe` with deadlines; `io::Copy` between files with `copy_file_range(2)`, from sockets with `splice(2)` and to them with `sendfile(2)` (`File::ReadFrom`/`WriteTo`), and `SyscallConn`; `os::Root`, Go's escape-proof directory handle, resolving every name with `openat(2)` one element at a time (symbolic links followed only within it), and `OpenInRoot`; `DirFS`, `Root::FS` and `CopyFS`; the `io/fs` functions over any `fs::FS` (`ReadFile`, `ReadDir`, `Stat`, `Sub`, `Glob`, `WalkDir`, `ReadLink`, `Lstat`); processes: `StartProcess`/`FindProcess`, `Process` with `Wait`/`Signal`/`Kill`/`Release`, `ProcessState`, pidfds waited for through the poller; the environment (`Environ`, `Setenv`, `LookupEnv`, `ExpandEnv`, ...), `Getpid` and friends, `Hostname`, `Executable`, `UserHomeDir`/`UserCacheDir`/`UserConfigDir`. Go's errors (`PathError`, `IsNotExist`, `errors::Is(err, fs::ErrNotExist)`) and `FileMode`/`FileInfo`/`DirEntry`. A file call parks only its fiber, on io_uring, or blocks its thread, which hands its processor on, as in Go; pipes park only their fiber |
| `os/exec` | `Command`/`CommandContext`, `Cmd` with `Run`/`Start`/`Wait`/`Output`/`CombinedOutput`, `StdinPipe`/`StdoutPipe`/`StderrPipe`, any `io::Reader`/`io::Writer` as standard streams (copied by fibers), `ExtraFiles`, `SysProcAttr` (namespaces, user namespace ID mappings, credentials, ambient capabilities, cgroups through `clone3(2)`, pidfds), `Cancel` and `WaitDelay`, `LookPath`, `ExitError`. Children are started with `clone(CLONE_VM\|CLONE_VFORK)`, safely from many threads, as Go does |
| `os/signal` | `Notify`, `Stop`, `Ignore`, `Ignored`, `Reset`, `NotifyContext`, delivering `os::Signal`s to channels; the handler only queues the signal, and a fiber delivers it |
| `testing/fstest`, `testing/iotest` | `MapFS`, the in-memory file system, and `TestFS`, which checks an `fs::FS` implementation; `iotest::TestReader` |
| `regexp`, `regexp/syntax` | Go's RE2 engine, ported: linear time in the input, with Go's NFA, backtracker and one-pass matchers chosen per expression and input, as Go chooses them. `Compile`/`MustCompile` (and the POSIX leftmost-longest variants), `Match`/`Find`/`FindAll` with their `Index`, `Submatch` and `String` forms (results share the input's memory, as Go's do), the `Reader` forms over an `io::RuneReader`, `ReplaceAll` with `$1`/`${name}` expansion, `ReplaceAllFunc`, `Expand`, `Split`, `SubexpNames`, `LiteralPrefix`, `QuoteMeta`; Unicode classes (`\p{Greek}`) from Go's tables. `regexp/syntax`'s parser, simplifier and compiler |
| `strings`, `bytes` | Go's functions over `gopp::string` and `slice<byte>`, Unicode-aware as Go's are: `Fields`, `Split`/`SplitN`/`SplitAfter`, `Cut`/`CutLast`/`CutPrefix`/`CutSuffix`, the `Trim` family, `EqualFold`, `ToUpper`/`ToLower`/`ToTitle` and their `Special` forms, `Replace`, `Map`, `Repeat`, `Index`/`IndexAny`/`IndexRune`/`IndexFunc` and the `Last` forms, `Count`, `ToValidUTF8`, and the iterators (`Lines`, `SplitSeq`, `FieldsSeq`, ...) as `std::generator`s. The parts they return share their input's buffer, as Go's substrings and subslices do. `bytes::Buffer` and `bytes::Reader`, and `strings::Builder` |
| `unicode`, `unicode/utf8` | Go's Unicode 17.0.0 tables, generated from Go's and constant-initialized: the categories, scripts and properties as `RangeTable`s (`unicode::Is(unicode::Han, r)`, `unicode::Scripts["Greek"]`), `IsLetter`/`IsUpper`/`IsSpace`/`IsPunct`/..., `ToUpper`/`ToLower`/`ToTitle`, `SimpleFold` and `TurkishCase`. `utf8`'s decoding, encoding, counting and validation, `constexpr` |
| `path/filepath` | Linux paths: `Abs`, `Rel`, `EvalSymlinks`, `Glob`, `Walk`/`WalkDir`, `IsLocal`, `Localize`, `SplitList`, and `path`'s lexical functions (`Join`, `Clean`, `Split`, `Base`, `Dir`, `Ext`, `Match`) |
| `io`, `bufio`, `fmt`, `strconv`, `errors`, `path`, `flag`, `syscall` | The everyday packages, with `io`'s `Pipe` (parking fibers), `MultiReader`, `MultiWriter`, `TeeReader`, `SectionReader`, `OffsetWriter` and `RuneReader`/`RuneScanner` (`bufio::Reader` reads runes too), and `fmt` on `std::format` (`fmt::W(err)` is Go's `%w`; `errors::Join` and `errors::Is`/`As` see wrapped lists) |

**Tested against Go's own tests.** 2019 unit tests, 1771 of them ported
from Go: `net/http`'s routing and ServeMux tests, its HTTP/2 server tests,
its file server tests (over HTTP/1.1, HTTPS and HTTP/2), its form, multipart request and `MaxBytesReader` tests, its Transport's gzip tests, `net/http/httputil`'s reverse proxy tests, `sync/atomic`'s tests, `container/heap`'s and `container/list`'s tests, `regexp`'s and `regexp/syntax`'s tests (with RE2's search tests and the AT&T POSIX tests), `mime`'s,
`encoding/json`'s scanner, encoding and decoding tests, HPACK's RFC 7541
vectors, `database/sql`'s pool, statement, transaction and conversion
tests against Go's fake driver, `text/template`'s lexer, parser and
executor tests, `os`'s and `io/fs`'s file, directory, pipe, error,
process, environment, zero-copy, `DirFS`, `CopyFS` and `os.Root` tests
(with the matrix that checks each `Root` operation against the plain
`os` one), `testing/fstest`'s, `path`'s and `errors`' tests, `syscall`'s
fork and exec tests (namespaces, ID mappings, ambient capabilities,
cgroups, chroot), `os/signal`'s tests, `os/exec`'s tests (which run
the test program again as their helper commands), `time`'s format,
calendar, monotonic clock and zoneinfo tests (against the system's
tzdata), `net`'s
address, dialing and listening, TCP, UDP, Unix socket, raw IP, interface,
DNS client and resolver configuration tests (against Go's fake DNS server
and testdata), `net/netip`'s and `dnsmessage`'s tests, `sort`'s, `slices`', `maps`' and `cmp`'s tests, `log`'s and `log/slog`'s tests, `io`'s pipe, multi-reader and section reader tests, `net/textproto`'s, `mime/quotedprintable`'s and `mime/multipart`'s tests, `strings`', `bytes`', `unicode`'s, `unicode/utf8`'s and `path/filepath`'s tests, and the test vectors and tests of `crypto/sha256`, `sha512`, `sha1`, `md5`, `hmac`, `rand` and `subtle`, `encoding/hex` and `encoding/base64`, `compress/gzip`'s, `compress/zlib`'s and `compress/flate`'s decoding tests and vectors, and `hash/crc32`'s and `hash/adler32`'s golden values. They keep Go's
test names and tables. The SQLite driver runs 59 tests ported from
`mattn/go-sqlite3`'s.

## Get started

go++ builds with [Jolt](https://github.com/srand/jolt), which builds every
dependency on first use: GCC 16, Boost.Context, liburing, OpenSSL,
SQLite and googletest. Your host compiler doesn't matter.

```sh
jolt build examples/hello          # the hello-world server (-cert/-key for HTTPS and HTTP/2, -h2c); Ctrl-C shuts it down gracefully
jolt build examples/fetch          # a small curl: fetch -i https://go.dev/, or a load generator: -n 1000 -c 50
jolt build examples/httpfileserver # serve a directory: httpfileserver -dir ./public (an os::Root: nothing outside it)
jolt build examples/proxy          # TCP proxy with splice(2): proxy -listen :8443 -to go.dev:443
jolt build examples/lookup         # concurrent DNS lookups: lookup go.dev example.com
jolt build examples/grep           # search a tree with fs::WalkDir, a pool of fibers and regexp: grep -dir src -ext .cpp 'TODO|FIXME'
jolt build examples/parallel       # run shell commands concurrently, like xargs -P: ... | parallel -j 8
jolt build examples/sandbox        # run a command in new namespaces, unprivileged: sandbox -net=false -- sh
jolt build examples/modules        # go++ imported as C++ modules
jolt build examples/prime          # concurrent prime sieve using goroutines and channels
jolt build examples/mandelbrot     # ASCII fractal rendered by a goroutine worker pool
jolt build examples/tcp-echo       # TCP echo server; connect with: nc localhost 8081
jolt build examples/udp-echo       # UDP echo server; try: nc -u -w1 localhost 8082
jolt build examples/tcp-chat       # multi-client chat room; connect from multiple terminals with nc localhost 8083
jolt build examples/template       # render a Markdown report from reflected C++ structs
jolt build examples/json-file      # a JSON task list, saved atomically: json-file -add "Buy milk"
jolt build examples/todo           # a JSON to-do API over SQLite: curl -d '{"title":"Buy milk"}' localhost:8080/todos
jolt build examples/search         # replicated search with a context deadline: search -replicas 1 golang
jolt build examples/sha256sum      # checksum files concurrently, like coreutils: sha256sum -a md5 *.cpp
jolt build examples/wordfreq       # the most common words, by Go's Unicode tables: wordfreq -n 10 README.md
jolt build examples/panic          # crash on purpose to see the goroutine tracebacks: GOTRACEBACK=all panic
jolt build test                    # run the unit tests
jolt build benchmark               # go++ vs Go, report in report/benchmark.md
```

The programs land in `artifacts/`, as
`artifacts/examples_hello/bin/examples_hello`. [examples/README.md](examples/README.md)
says what each example shows, and where to start.

| Task | What it builds |
|---|---|
| `jolt build gopp` | the library: `libgopp.a` and `include/gopp/` |
| `jolt build gopp/sqlite3` | the SQLite driver: `libgopp_sqlite3.a` and `<gopp/sqlite3.hpp>`, with SQLite 3.54 |
| `jolt build examples` | the example programs |
| `jolt build gopp/tests` | the unit test program |
| `jolt build test` | runs the unit tests |
| `jolt build benchmarks/files-gopp` | `files -op stat\|read\|open\|write\|readdir -c 64 -d 3s`: file system calls per second from many fibers; run with `GOPP_IOURING=0` to compare |
| `jolt build benchmark` | runs `examples/hello` and the same server in Go 1.27 (`benchmarks/hello-go`) under the same load; publishes `report/benchmark.md` and `.json` |

Requirements: Linux, x86-64. go++ is C++26 with static reflection (P2996),
which GCC 16 implements (`-freflection`); Clang doesn't yet. Programs link
libstdc++ statically.

### As C++ modules

Each header is also a named module: `<gopp/net/http.hpp>` is
`gopp.net.http`, `<gopp/text/template.hpp>` is `gopp.text.tmpl`, and
`gopp` imports them all. A module exports the modules it depends on, as a
header includes them, but not the standard library: import that too.

```cpp
import std;
import gopp.encoding.json;
import gopp.fmt;
```

`jolt build gopp` publishes the compiled interfaces, with `std` and
`std.compat`, so a Jolt task that requires `gopp` can import them; they need
the same compiler and flags (`examples/modules`). Don't `#include` a go++
header after an `import` in the same file: GCC can't merge the declarations
in that order.

The interfaces are in `modules/`, one per header. In module mode
(`GOPP_MODULE`), a header skips its `#include`s, and the interface imports
`std.compat` and the modules of the go++ headers it included.

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
- **Writing to a broken pipe returns EPIPE**, as in Go: the runtime handles
  SIGPIPE, except that a write to a broken standard output or error still
  ends the program with the signal, so `prog | head` exits quietly. A
  program started with SIGPIPE ignored, or that installs its own handler,
  keeps it; with `signal::Notify(c, syscall::SIGPIPE_)` the channel gets it.
- **Signals nobody is notified of keep their default action.** Go's
  runtime handles every signal and ignores some, such as SIGUSR1, when no
  channel asks for them; go++ installs its handler only for the signals
  `signal::Notify` asks for, so an unexpected SIGUSR1 ends the program as
  in C. Signals the program started with ignored stay ignored (Go keeps
  only SIGHUP and SIGINT so).
- **Crashes print the goroutines' stacks**, as Go's do: `panic`, an
  exception that escapes a fiber or a thread, a fatal error, a segfault,
  SIGFPE, SIGABRT (`abort`, a failed `assert`), Ctrl-\\ (SIGQUIT) and the
  other crash signals print the message and `goroutine N [chan receive]:`
  blocks with each function, its file and line, and `created by`, then exit
  with status 2. An exception is reported from where it was thrown, before
  any destructor runs. `GOTRACEBACK` works as in Go: `none`, `single` (the
  default: panics print the crashing goroutine, fatal errors and signals
  all of them), `all`, `system` (with the runtime's frames) and `crash`
  (dies of SIGABRT, for a core dump). Threads that use the runtime, the
  main thread among them, are listed as `thread TID`. Frames print as
  `name(...)`, without Go's argument words, and the files and lines come
  from the debug information (`-g`). Without it the names come from the
  symbol table, and where there is no file and line, a frame prints its pc
  and its address in the program, as in
  `? pc=0x40acb4 (/usr/bin/prog+0x40acb3)`, which
  `addr2line -e prog 0x40acb3` resolves on a copy that isn't stripped. With optimization, a call inlined into
  a frame shows the plain name the debug information gives it, such as
  `operator()`, and a tail call leaves no frame.
- A few spellings differ from Go. `io.EOF` is `io::EOF_` and `os.O_RDONLY`
  is `os::O_RDONLY_` (and so on for `os`'s `O_` flags), because C headers
  define Go's names as macros; so are `syscall::SIGTERM_` and the other
  signals, and the errno errors such as `syscall::ECHILD_`. And `url::Error` can't declare `Error()`,
  because C++ reserves a class's own name for its constructors: call it
  through `gopp::error`, or use `message()`; so does `exec::Error`. For the
  same reason `os::Signal` is a value type, like `gopp::error`, and the
  interface behind it is `os::signal_interface`.

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

`jolt build test:leaks=true,repeat=2` checks every test for leaks: built
with `GOPP_DEBUG_LEAKS`, go++ records each live `gopp::object`, and a test
fails if it leaves objects alive, which are listed by type. The first round
is a warm-up, so that objects kept for good (default clients, caches)
don't count.

`GOPP_SCHEDTRACE=<ms>` prints the scheduler's counters to standard error
every `<ms>` milliseconds, like Go's `GODEBUG=schedtrace`. It shows fibers
scheduled, steals, network polls, threads parked and woken, and I/O that
would block. `GOPP_STACK_SIZE` sets the fiber stack size.

File system calls go through io_uring when the kernel allows it: a fiber
submits its call to its thread's ring and parks, and its thread runs other
fibers until the completion comes in, which the network poller picks up
from the rings' descriptors. That's done only when the thread has other
fibers to run. With nothing else to run, a direct system call is faster:
the kernel hands most of these calls (`statx`, `unlinkat`, reads on tmpfs,
...) to a worker thread, whose wakeup costs more than the call. Calls on
regular files and paths take this route; pipes and terminals that aren't
on the poller don't. `GOPP_IOURING=0` turns io_uring off, and without it
file calls are blocking calls, whose thread hands its processor to
another. `benchmarks/files-gopp` compares the two: with 64 fibers, stat,
open and read calls run 2 to 4 times faster through io_uring (writes too,
unless they wait for the disk), and with one fiber as fast as directly.

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

- **HTTP/2** has no server push, RFC 9218 priorities or trailers yet. The
  server ignores priorities and schedules streams round-robin, as Go's does
  by default.
- **Preemption moves the processor, not the fiber.** A fiber that computes
  for more than 10 ms while other fibers wait loses its processor to
  another thread, which runs them. The fiber itself is never switched out
  mid-code: it keeps running on its own thread, so `errno`, `thread_local`
  values and held locks stay valid, and it gets a processor back when it
  next blocks or yields. Meanwhile more threads than `GOMAXPROCS` can be
  busy, at most twice as many from preemption: sysmon takes no more
  processors while `GOMAXPROCS` fibers run without one. As in Go, system calls made without `runtime::blocking` may fail
  with `EINTR` from SIGURG. `GODEBUG=asyncpreemptoff=1` turns preemption
  off.
- **Fixed-size fiber stacks**, set by `GOPP_STACK_SIZE` (256 KiB of
  virtual memory by default).
- **One global timer heap**, where Go keeps one per P.
- **Sockets and pipes use epoll**; io_uring carries only file system calls so
  far.
- **Name resolution is Go's own resolver only**, as `GODEBUG=netdns=go`
  has it: `/etc/hosts`, then DNS. Name service switch modules such as
  `mdns4` or `myhostname` aren't consulted, and DNS SVCB and HTTPS records
  parse as unknown records.
- **x86-64 only for now.** The code supports ARM64, but it isn't built or
  tested there yet.
- **`flag` has only the Var forms** (`StringVar`, `IntVar`, ...).
- **No protocol upgrades through `ReverseProxy`** (WebSocket and other
  101 Switching Protocols responses): the server has no `Hijacker` yet.
- **Request contexts** are canceled when the handler returns or a write to
  the client fails, not when the client disconnects while the handler is
  idle.
