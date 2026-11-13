// The net/http package: an HTTP/1.1 server, ported from Go's net/http.
//
//     int main() {
//         http::ServeMux mux;
//         mux.HandleFunc("GET /hello/{name}", [](http::ResponseWriter& w, http::Request& r) {
//             fmt::Print(w, "Hello, {}!\n", r.PathValue("name"));
//         });
//         http::Server srv{.Addr = ":8080", .Handler = make_ptr<http::ServeMux>(std::move(mux))};
//         if (auto err = srv.ListenAndServe()) fmt::Println("{}", err);
//     }
//
// Every connection is served on its own fiber, so handlers are written as
// plain blocking code.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/context.hpp>
#include <gopp/crypto/tls.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/net.hpp>
#include <gopp/net/url.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::http {

namespace internal {
struct mux_access;
}

// ---------------------------------------------------------------------------
// Constants

inline constexpr std::string_view MethodGet = "GET";
inline constexpr std::string_view MethodHead = "HEAD";
inline constexpr std::string_view MethodPost = "POST";
inline constexpr std::string_view MethodPut = "PUT";
inline constexpr std::string_view MethodPatch = "PATCH";
inline constexpr std::string_view MethodDelete = "DELETE";
inline constexpr std::string_view MethodConnect = "CONNECT";
inline constexpr std::string_view MethodOptions = "OPTIONS";
inline constexpr std::string_view MethodTrace = "TRACE";

enum status : int {
    StatusContinue = 100,
    StatusSwitchingProtocols = 101,
    StatusOK = 200,
    StatusCreated = 201,
    StatusAccepted = 202,
    StatusNoContent = 204,
    StatusPartialContent = 206,
    StatusMovedPermanently = 301,
    StatusFound = 302,
    StatusSeeOther = 303,
    StatusNotModified = 304,
    StatusTemporaryRedirect = 307,
    StatusPermanentRedirect = 308,
    StatusBadRequest = 400,
    StatusUnauthorized = 401,
    StatusForbidden = 403,
    StatusNotFound = 404,
    StatusMethodNotAllowed = 405,
    StatusRequestTimeout = 408,
    StatusConflict = 409,
    StatusGone = 410,
    StatusLengthRequired = 411,
    StatusPreconditionFailed = 412,
    StatusRequestEntityTooLarge = 413,
    StatusUnsupportedMediaType = 415,
    StatusRequestedRangeNotSatisfiable = 416,
    StatusExpectationFailed = 417,
    StatusTooManyRequests = 429,
    StatusRequestHeaderFieldsTooLarge = 431,
    StatusInternalServerError = 500,
    StatusNotImplemented = 501,
    StatusBadGateway = 502,
    StatusServiceUnavailable = 503,
    StatusGatewayTimeout = 504,
    StatusHTTPVersionNotSupported = 505,
};

// http.StatusText
std::string_view StatusText(int code) noexcept;

// http.Protocols: the protocols a Server accepts or a Transport uses.
// HTTP/2 is negotiated with ALPN over TLS; unencrypted HTTP/2 (h2c) is
// spoken with prior knowledge: the client starts with the HTTP/2 preface.
class Protocols {
public:
    bool HTTP1() const noexcept { return bits_ & http1_bit; }
    void SetHTTP1(bool ok) noexcept { Set(http1_bit, ok); }
    bool HTTP2() const noexcept { return bits_ & http2_bit; }
    void SetHTTP2(bool ok) noexcept { Set(http2_bit, ok); }
    bool UnencryptedHTTP2() const noexcept { return bits_ & h2c_bit; }
    void SetUnencryptedHTTP2(bool ok) noexcept { Set(h2c_bit, ok); }
    // "{HTTP1,HTTP2,UnencryptedHTTP2}"
    string String() const;

private:
    static constexpr uint8 http1_bit = 1, http2_bit = 2, h2c_bit = 4;
    void Set(uint8 bit, bool ok) noexcept { bits_ = ok ? (bits_ | bit) : (bits_ & ~bit); }
    uint8 bits_ = 0;
};

// The protocols of a nil Server.Protocols or Transport.Protocols: HTTP/1
// and HTTP/2 over TLS.
Protocols DefaultProtocols() noexcept;

// http.DefaultMaxHeaderBytes
inline constexpr int DefaultMaxHeaderBytes = 1 << 20;

extern const gopp::error ErrServerClosed;
extern const gopp::error ErrBodyNotAllowed;
extern const gopp::error ErrContentLength;
extern const gopp::error ErrBodyReadAfterClose;
extern const gopp::error ErrHandlerTimeout;
extern const gopp::error ErrAbortHandler;

// ---------------------------------------------------------------------------
// Header

// http.CanonicalHeaderKey: "content-type" -> "Content-Type".
string CanonicalHeaderKey(std::string_view key);

// http.Header: a multimap of canonical keys to values, in insertion order.
// Lookups ignore case.
class Header {
public:
    struct entry {
        string key;
        string value;
    };

    // The first value of key, or "".
    string Get(std::string_view key) const;
    // All values of key.
    std::vector<string> Values(std::string_view key) const;
    // Replaces the values of key.
    void Set(std::string_view key, string value);
    // Appends a value to key.
    void Add(std::string_view key, string value);
    void Del(std::string_view key);
    bool Has(std::string_view key) const;
    // Alias for add, for map-style code.
    void emplace(std::string_view key, string value) { Add(key, std::move(value)); }

    // Adds an entry whose key is canonical already.
    void AddCanonical(string key, string value) { entries_.push_back({std::move(key), std::move(value)}); }

    // Writes the header in wire format, sorted by key, without the keys in
    // exclude (canonical).
    gopp::error Write(io::Writer& w) const;
    void WriteTo(std::string& out, std::initializer_list<std::string_view> exclude = {}) const;

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    void clear() noexcept { entries_.clear(); }
    auto begin() const noexcept { return entries_.begin(); }
    auto end() const noexcept { return entries_.end(); }

private:
    std::vector<entry> entries_;
};

// ---------------------------------------------------------------------------
// Request

class Request : public object {
public:
    string Method;
    url::URL URL;
    string Proto = string::from_static("HTTP/1.1");
    int ProtoMajor = 1;
    int ProtoMinor = 1;
    http::Header Header;
    // The request body; never nil on server requests (empty when there is
    // none). The server closes it.
    ptr<io::ReadCloser> Body;
    // -1 if unknown (chunked).
    int64 ContentLength = 0;
    std::vector<string> TransferEncoding;
    // Whether to close the connection after the response.
    bool Close = false;
    string Host;
    string RemoteAddr;
    string RequestURI;
    // The ServeMux pattern that matched.
    string Pattern;
    // The TLS connection the request came on, for server requests over TLS.
    std::optional<tls::ConnectionState> TLS;

    Request() = default;
    Request(const Request&);
    Request& operator=(const Request&) = delete;
    ~Request() override;

    // The request's context: canceled when the handler returns or the
    // client connection fails.
    ptr<context::Context> Context() const;
    // A shallow copy with ctx as its context (r.WithContext).
    ptr<Request> WithContext(ptr<context::Context> ctx) const;

    // The value of a path wildcard of the matched pattern, or "".
    string PathValue(std::string_view name) const;
    void SetPathValue(std::string_view name, string value);

    // The first value of key in the URL query (r.FormValue without bodies).
    string FormValue(std::string_view key) const;
    string UserAgent() const { return Header.Get("User-Agent"); }
    string Referer() const { return Header.Get("Referer"); }
    bool ProtoAtLeast(int major, int minor) const noexcept {
        return ProtoMajor > major || (ProtoMajor == major && ProtoMinor >= minor);
    }

    // Cancels the context, if one was created.
    void CancelContext() const;

private:
    friend class ServeMux;
    friend class Transport;
    friend std::pair<ptr<Request>, gopp::error> NewRequestWithContext(ptr<context::Context>, std::string_view,
                                                                         std::string_view, ptr<io::Reader>);
    mutable ptr<context::Context> ctx_;
    mutable context::CancelFunc cancel_;
    std::vector<std::pair<string, string>> path_values_;
};

// ---------------------------------------------------------------------------
// ResponseWriter, Handler

// http.ResponseWriter
struct ResponseWriter : public io::Writer {
    // The header to send. Changes after WriteHeader or the first write
    // have no effect.
    virtual http::Header& Header() = 0;
    // Sends the status line and header.
    virtual void WriteHeader(int status_code) = 0;
};

// http.Flusher: implemented by the server's ResponseWriter.
struct Flusher : public virtual object {
    virtual void Flush() = 0;
};

// http.Handler
struct Handler : public virtual object {
    virtual void ServeHTTP(ResponseWriter& w, Request& r) = 0;
};

using handler_fn = std::function<void(ResponseWriter&, Request&)>;

// http.HandlerFunc
class HandlerFunc final : public Handler {
public:
    explicit HandlerFunc(handler_fn f) : f_(std::move(f)) {}
    void ServeHTTP(ResponseWriter& w, Request& r) override { f_(w, r); }

private:
    handler_fn f_;
};


// http.Error: replies with the message as plain text and the code.
void Error(ResponseWriter& w, std::string_view msg, int code);
// http.NotFound
void NotFound(ResponseWriter& w, Request& r);
ptr<Handler> NotFoundHandler();
// http.Redirect
void Redirect(ResponseWriter& w, Request& r, std::string_view url, int code);
ptr<Handler> RedirectHandler(string url, int code);
// http.StripPrefix
ptr<Handler> StripPrefix(string prefix, ptr<Handler> h);

// http.DetectContentType: the MIME type of data, from its first 512 bytes.
string DetectContentType(std::span<const byte> data);

// http.TimeFormat: the format of times in HTTP headers, always in UTC, as
// time::Time::FormatHTTP writes them.
inline constexpr std::string_view TimeFormat = "Mon, 02 Jan 2006 15:04:05 GMT";

// http.ParseTime: parses a time header (Date, If-Modified-Since, ...) in
// one of the three formats HTTP/1.1 allows: TimeFormat, RFC 850 ("Monday,
// 02-Jan-06 15:04:05 GMT") or ANSI C's asctime ("Mon Jan  2 15:04:05 2006").
std::pair<time::Time, gopp::error> ParseTime(std::string_view text);

// ---------------------------------------------------------------------------
// File servers
//
//     http::Handle("/", http::FileServer(http::Dir("/srv/www")));
//
//     auto [root, err] = os::OpenRoot("/srv/www");     // can't be escaped
//     http::Handle("/", http::FileServerFS(root->FS()));

// http.File: a file of a FileSystem, as FileServer serves it. The methods
// behave as os::File's do.
struct File : public io::Reader, public io::Seeker, public io::Closer {
    // Reads up to count entries of a directory (all if count <= 0); with
    // count > 0, io::EOF_ at the end.
    virtual std::pair<std::vector<ptr<fs::FileInfo>>, gopp::error> Readdir(int count) = 0;
    virtual std::pair<ptr<fs::FileInfo>, gopp::error> Stat() = 0;
};

// http.FileSystem: a collection of files, named by slash-separated paths
// whatever the system's separator. The FS adapter turns an fs::FS into one.
struct FileSystem : public virtual object {
    virtual std::pair<ptr<File>, gopp::error> Open(std::string_view name) = 0;
};

// http.Dir: the FileSystem of the native file system tree at a directory
// (an empty Dir is "."). It follows symbolic links that point out of the
// tree, and serves names that start with a period, such as .git: serve an
// os::Root's FS with FileServerFS to keep requests inside the tree.
class Dir final : public FileSystem {
public:
    explicit Dir(string dir = {}) noexcept : dir_(std::move(dir)) {}
    // Opens the file name, made relative to the directory with path::Clean,
    // for reading.
    std::pair<ptr<File>, gopp::error> Open(std::string_view name) override;
    const string& String() const noexcept { return dir_; }

private:
    string dir_;
};

// http.FS: fsys as a FileSystem, for FileServer. Its files must be
// io::Seekers; os::DirFS's, os::Root::FS's and fstest::MapFS's are.
ptr<FileSystem> FS(ptr<fs::FS> fsys);

// http.FileServer: a handler that serves the files of root, by request
// path: files with ServeContent (ranges, conditional requests, the type
// from the extension or the content), and directories as their
// index.html or as an HTML listing. Paths are cleaned; a directory's path
// is redirected to end in a slash, a file's not to, and ".../index.html"
// to ".../".
ptr<Handler> FileServer(ptr<FileSystem> root);
inline ptr<Handler> FileServer(const Dir& root) { return FileServer(make_ptr<Dir>(root)); }

// http.FileServerFS: FileServer(FS(root)).
ptr<Handler> FileServerFS(ptr<fs::FS> root);

// http.ServeFile: replies with the named file or directory of the native
// file system; a relative name is relative to the working directory. It
// doesn't use r.URL.Path to find the file, but refuses requests whose path
// has a ".." element (400), and redirects ".../index.html" to ".../".
void ServeFile(ResponseWriter& w, Request& r, std::string_view name);

// http.ServeFileFS: ServeFile with the file name of fsys.
void ServeFileFS(ResponseWriter& w, Request& r, ptr<fs::FS> fsys, std::string_view name);

namespace detail {
void serve_content(ResponseWriter& w, Request& r, std::string_view name, time::Time modtime, io::Reader& content,
                   io::Seeker& seeker);
}

// http.ServeContent: replies with content, which must be a Reader and a
// Seeker (an os::File, a strings::NewReader): its size is found by seeking
// to the end. Over io::Copy, it handles Range requests (206 Partial
// Content, multipart/byteranges for several ranges), the preconditions
// If-Match, If-Unmodified-Since, If-None-Match, If-Modified-Since and
// If-Range (with the ETag header that w has, if any, and modtime), and
// sets Content-Type, unless w has one, from name's extension
// (mime::TypeByExtension) or else from the content (DetectContentType).
// A modtime that is zero or the Unix epoch is left out; otherwise it's the
// Last-Modified header. On errors, such as an unsatisfiable range, it
// replies with the error and drops the Cache-Control, Content-Encoding,
// Etag and Last-Modified headers that were meant for the content.
template <typename RS>
    requires std::derived_from<RS, io::Reader> && std::derived_from<RS, io::Seeker>
void ServeContent(ResponseWriter& w, Request& r, std::string_view name, time::Time modtime, RS& content) {
    detail::serve_content(w, r, name, modtime, content, content);
}

// ---------------------------------------------------------------------------
// ServeMux

// http.ServeMux with Go 1.22 patterns: "[METHOD ][HOST]/[PATH]", where path
// segments may be wildcards: "{name}", "{name...}" for the rest of the path,
// "{$}" for the end of the path. The most specific pattern wins.
class ServeMux final : public Handler {
public:
    ServeMux();
    ServeMux(ServeMux&&) noexcept;
    ServeMux& operator=(ServeMux&&) noexcept;
    ~ServeMux() override;

    // Registers h for pattern. Panics if the pattern is invalid, h is nil,
    // or the pattern conflicts with a registered one; the message names
    // where both were registered.
    void Handle(std::string_view pattern, ptr<http::Handler> h, std::source_location loc = std::source_location::current());

    template <typename F>
    void HandleFunc(std::string_view pattern, F&& f, std::source_location loc = std::source_location::current()) {
        handler_fn fn(std::forward<F>(f));
        Handle(pattern, fn ? make_ptr<HandlerFunc>(std::move(fn)) : nullptr, loc);
    }

    void ServeHTTP(ResponseWriter& w, Request& r) override;

    // The handler for r and the pattern that matched (mux.Handler). Sets
    // r's path values.
    std::pair<ptr<http::Handler>, string> Handler(Request& r) const;

private:
    friend struct internal::mux_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

ptr<ServeMux> NewServeMux();

// http.DefaultServeMux and the package-level functions that use it.
ServeMux& DefaultServeMux();
void Handle(std::string_view pattern, ptr<Handler> h, std::source_location loc = std::source_location::current());
template <typename F>
void HandleFunc(std::string_view pattern, F&& f, std::source_location loc = std::source_location::current()) {
    DefaultServeMux().HandleFunc(pattern, std::forward<F>(f), loc);
}

// ---------------------------------------------------------------------------
// Server

namespace detail {
struct server_state;
}

// http.Server. An aggregate, for designated initializers:
//
//     http::Server srv{.Addr = ":8080", .Handler = mux};
struct Server {
    // "host:port"; ":http" if empty.
    string Addr{};
    // nil means DefaultServeMux().
    ptr<http::Handler> Handler{};
    // Time limit for reading a request, including its body.
    time::Duration ReadTimeout{};
    // Time limit for reading a request's header; ReadTimeout if zero.
    time::Duration ReadHeaderTimeout{};
    // Time limit for writing a response.
    time::Duration WriteTimeout{};
    // Time limit for waiting for the next request; ReadTimeout if zero.
    time::Duration IdleTimeout{};
    // Maximum size of a request header; DefaultMaxHeaderBytes if 0.
    int MaxHeaderBytes = 0;
    bool DisableKeepAlives = false;
    // TLS settings for ListenAndServeTLS and ServeTLS. ALPN offers h2
    // and http/1.1 unless NextProtos is set.
    std::optional<tls::Config> TLSConfig{};
    // The protocols to accept; HTTP/1 and HTTP/2 over TLS if unset.
    std::optional<http::Protocols> Protocols{};

    // Listens on addr and serves until closed. Always returns an error;
    // ErrServerClosed after close() or shutdown().
    gopp::error ListenAndServe();
    // Serves connections from l.
    gopp::error Serve(ptr<net::Listener> l);
    // Listens on addr (":https" if empty) and serves HTTPS with the
    // certificate and key in the PEM files, or those of TLSConfig.
    gopp::error ListenAndServeTLS(std::string_view cert_file, std::string_view key_file);
    // Serves HTTPS on connections from l.
    gopp::error ServeTLS(ptr<net::Listener> l, std::string_view cert_file, std::string_view key_file);
    // Closes the listeners and all connections now.
    gopp::error Close();
    // Closes the listeners, then waits for active connections to become
    // idle and closes them, until ctx is done.
    gopp::error Shutdown(ptr<context::Context> ctx);

    // Internal state, shared with connections.
    std::shared_ptr<detail::server_state> state_ = nullptr;

    std::shared_ptr<detail::server_state> state();
};

// http.ListenAndServe
gopp::error ListenAndServe(string addr, ptr<Handler> handler);
// http.ListenAndServeTLS
gopp::error ListenAndServeTLS(string addr, std::string_view cert_file, std::string_view key_file, ptr<Handler> handler);

// ---------------------------------------------------------------------------
// Client
//
//     auto [resp, err] = http::Get("http://example.com/");
//     if (err) return err;
//     auto [body, rerr] = io::ReadAll(*resp->Body);
//     resp->Body->Close();
//
// https:// URLs negotiate HTTP/2 or HTTP/1.1 with ALPN.

// http.Response
class Response : public object {
public:
    string Status;  // "200 OK"
    int StatusCode = 0;
    string Proto;  // "HTTP/1.1"
    int ProtoMajor = 1;
    int ProtoMinor = 1;
    http::Header Header;
    // The response body. The caller must read it to the end or close it;
    // a body read to the end returns its connection to the pool.
    ptr<io::ReadCloser> Body;
    // -1 if unknown.
    int64 ContentLength = -1;
    std::vector<string> TransferEncoding;
    // Whether the server closes the connection after this response.
    bool Close = false;
    // The TLS connection the response came on, for https.
    std::optional<tls::ConnectionState> TLS;
    // The request that was sent for this response.
    ptr<http::Request> Request;
};

// http.NewRequest
std::pair<ptr<Request>, gopp::error> NewRequest(std::string_view method, std::string_view url, ptr<io::Reader> body = nullptr);
// http.NewRequestWithContext
std::pair<ptr<Request>, gopp::error> NewRequestWithContext(ptr<context::Context> ctx, std::string_view method,
                                                              std::string_view url, ptr<io::Reader> body = nullptr);

// http.RoundTripper: sends one request and returns its response.
struct RoundTripper : public virtual object {
    virtual std::pair<ptr<Response>, gopp::error> RoundTrip(Request& req) = 0;
};

namespace detail {
class persist_conn;
}

// http.Transport: HTTP/1.1 with pooled keep-alive connections.
class Transport final : public RoundTripper {
public:
    // Idle connections kept per host (http.DefaultMaxIdleConnsPerHost is 2).
    int MaxIdleConnsPerHost = 2;
    // Total idle connections; 0 means no limit.
    int MaxIdleConns = 100;
    bool DisableKeepAlives = false;
    // Time limit for connecting; 0 means none.
    time::Duration DialTimeout{};
    // Time limit for the response header after the request is written.
    time::Duration ResponseHeaderTimeout{};
    // TLS settings for https; the system roots if unset.
    std::optional<tls::Config> TLSClientConfig{};
    // Time limit for the TLS handshake; none if 0.
    time::Duration TLSHandshakeTimeout = 10 * time::Second;
    // The protocols to use; HTTP/1 and HTTP/2 over TLS if unset.
    std::optional<http::Protocols> Protocols{};

    Transport();
    ~Transport() override;

    std::pair<ptr<Response>, gopp::error> RoundTrip(Request& req) override;
    // Closes the idle connections.
    void CloseIdleConnections();

    struct impl;  // the connection pool, shared with connections in use

private:
    std::shared_ptr<impl> impl_;
};

// http.DefaultTransport
const ptr<Transport>& DefaultTransport();

// http.Client: follows redirects and applies the timeout around a
// RoundTripper.
class Client {
public:
    // nil means DefaultTransport().
    ptr<RoundTripper> Transport{};
    // Time limit for a request, including redirects and reading the body;
    // 0 means none.
    time::Duration Timeout{};
    // Redirects followed before giving up (Go stops after 10).
    int MaxRedirects = 10;

    // client.Do ("do" is a C++ keyword).
    std::pair<ptr<Response>, gopp::error> Do(ptr<Request> req);
    std::pair<ptr<Response>, gopp::error> Get(std::string_view url);
    std::pair<ptr<Response>, gopp::error> Head(std::string_view url);
    std::pair<ptr<Response>, gopp::error> Post(std::string_view url, std::string_view content_type, ptr<io::Reader> body);
    // Closes the transport's idle connections, if it is a Transport.
    void CloseIdleConnections();
};

// http.DefaultClient
Client& DefaultClient();

// http.Get, http.Head, http.Post: with DefaultClient().
std::pair<ptr<Response>, gopp::error> Get(std::string_view url);
std::pair<ptr<Response>, gopp::error> Head(std::string_view url);
std::pair<ptr<Response>, gopp::error> Post(std::string_view url, std::string_view content_type, ptr<io::Reader> body);

}  // namespace gopp::http
