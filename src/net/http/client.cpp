// The HTTP client, after Go's net/http/client.Go and transport.Go.
//
// Go's Transport runs a read and a write goroutine per connection; here a
// round trip writes the request and reads the response on the calling
// fiber, which parks while the network is busy, and the response body
// returns the connection to the pool once it is read to the end.
#include "ascii.hpp"
#include "internal.hpp"

#include <gopp/compress/gzip.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace gopp::http {

namespace detail {

class persistConn final : public object {
public:
    persistConn(std::string key, ptr<net::Conn> c)
        : key(std::move(key)), conn(std::move(c)), br(make_ptr<bufio::Reader>(conn, 4096)), bw(conn, 4096) {}

    std::string key;
    ptr<net::Conn> conn;
    ptr<bufio::Reader> br;
    bufio::Writer bw;
    bool hasDeadline = false;
    std::optional<tls::ConnectionState> tls;
};

}  // namespace detail

using detail::persistConn;

struct Transport::impl {
    // The TLS config for https, compiled once and shared by connections.
    std::mutex tlsMu;
    std::optional<tls::Config> TLSConfig;

    tls::Config clientTlsConfig(const Transport& t) {
        std::lock_guard l(tlsMu);
        if (!TLSConfig) {
            TLSConfig = t.TLSClientConfig.value_or(tls::Config{});
            TLSConfig->ctx_ = nullptr;
            if (TLSConfig->NextProtos.empty()) {
                http::Protocols p = t.Protocols.value_or(DefaultProtocols());
                if (p.HTTP2() && internal::http2ClientEnabled) TLSConfig->NextProtos.push_back(string::fromStatic("h2"));
                if (p.HTTP1()) TLSConfig->NextProtos.push_back(string::fromStatic("http/1.1"));
            }
        }
        return *TLSConfig;
    }

    std::mutex mu;
    std::unordered_map<std::string, std::vector<ptr<persistConn>>> idle;
    int nidle = 0;

    // HTTP/2 connections, shared by concurrent requests.
    sync::Mutex h2Mu;  // fiber-aware: held while asking connections, which may park
    std::unordered_map<std::string, std::vector<ptr<internal::http2ClientConn>>> h2;

    // An HTTP/2 connection to key that can take a request now.
    ptr<internal::http2ClientConn> h2Get(const std::string& key) {
        std::lock_guard l(h2Mu);
        auto it = h2.find(key);
        if (it == h2.end()) return nullptr;
        std::erase_if(it->second, [](auto& cc) { return cc->closed(); });
        for (auto& cc : it->second) {
            if (cc->canTakeNewRequest()) return cc;
        }
        return nullptr;
    }

    void h2Put(const std::string& key, ptr<internal::http2ClientConn> cc) {
        std::lock_guard l(h2Mu);
        h2[key].push_back(std::move(cc));
    }

    void h2CloseIdle() {
        std::lock_guard l(h2Mu);
        for (auto& [key, list] : h2) std::erase_if(list, [](auto& cc) { return cc->closeIfIdle() || cc->closed(); });
    }

    ptr<persistConn> getIdle(const std::string& key) {
        std::lock_guard l(mu);
        auto it = idle.find(key);
        if (it == idle.end() || it->second.empty()) return nullptr;
        ptr<persistConn> pc = std::move(it->second.back());  // most recently used
        it->second.pop_back();
        nidle--;
        return pc;
    }

    // Returns false if the pool is full; the caller closes pc then.
    bool putIdle(const ptr<persistConn>& pc, int maxPerHost, int maxTotal) {
        std::lock_guard l(mu);
        auto& list = idle[pc->key];
        if (maxPerHost <= 0) maxPerHost = 2;
        if (static_cast<int>(list.size()) >= maxPerHost) return false;
        if (maxTotal > 0 && nidle >= maxTotal) return false;
        list.push_back(pc);
        nidle++;
        return true;
    }

    void closeIdle() {
        std::unordered_map<std::string, std::vector<ptr<persistConn>>> old;
        {
            std::lock_guard l(mu);
            old.swap(idle);
            nidle = 0;
        }
        for (auto& [key, list] : old) {
            for (auto& pc : list) pc->conn->Close();
        }
        h2CloseIdle();
    }
};

namespace {

const gopp::error errMalformedResponse = errors::New("malformed HTTP response");

// The body of a response. Reading it to the end returns the connection to
// the pool; closing it early closes the connection, as in Go. Close may
// come from another fiber during a Read: a Read holds its own references to
// the connection, so that Close only closes it, which ends the Read.
class clientBody final : public io::ReadCloser {
public:
    clientBody(std::shared_ptr<Transport::impl> t, ptr<persistConn> pc, ptr<io::Reader> src, io::LimitedReader* lr,
                bool reusable, int maxPerHost, int maxTotal)
        : t_(std::move(t)),
          pc_(std::move(pc)),
          src_(std::move(src)),
          lr_(lr),
          reusable_(reusable),
          maxPerHost_(maxPerHost),
          maxTotal_(maxTotal) {}

    ~clientBody() override {
        if (!done_) finish(false);
    }

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        ptr<io::Reader> src;
        ptr<persistConn> pc;  // keeps the connection alive during the read
        {
            std::lock_guard l(mu_);
            if (done_) return {0, closed_ ? ErrBodyReadAfterClose : io::EOF_};
            src = src_;
            pc = pc_;
        }
        auto [n, err] = src->Read(p);
        std::lock_guard l(mu_);
        if (done_) return {n, closed_ ? ErrBodyReadAfterClose : err ? err : io::EOF_};
        if (!err && lr_ && lr_->Remaining() == 0) {
            // The last bytes: release the connection now, so that the next
            // request can use it before the caller reads EOF.
            finish(true);
            return {n, n == 0 ? io::EOF_ : nullptr};
        }
        if (err == io::EOF_) {
            bool complete = !lr_ || lr_->Remaining() == 0;
            finish(complete && reusable_);
            if (!complete) err = io::ErrUnexpectedEOF;
        } else if (err) {
            finish(false);
        }
        return {n, err};
    }

    gopp::error Close() override {
        std::lock_guard l(mu_);
        closed_ = true;
        if (!done_) finish(false);
        return nullptr;
    }

private:
    // Called with mu_ held, or from the destructor.
    void finish(bool reuse) {
        done_ = true;
        ptr<persistConn> pc = std::move(pc_);
        src_.reset();
        if (!pc) return;
        if (reuse && reusable_) {
            if (pc->hasDeadline) {
                pc->conn->SetDeadline(time::Time{});
                pc->hasDeadline = false;
            }
            if (t_->putIdle(pc, maxPerHost_, maxTotal_)) return;
        }
        pc->conn->Close();
    }

    sync::Mutex mu_;
    std::shared_ptr<Transport::impl> t_;
    ptr<persistConn> pc_;
    ptr<io::Reader> src_;
    io::LimitedReader* lr_;
    bool reusable_;
    int maxPerHost_;
    int maxTotal_;
    bool done_ = false;
    bool closed_ = false;
};

string urlErrorOp(std::string_view method) {
    // "GET" -> "Get", as Go names client errors.
    if (method.empty()) return string::fromStatic("Get");
    return string(method.substr(0, 1)) + ascii::ToLowerBytes(method.substr(1));
}

gopp::error urlError(std::string_view method, const url::URL& u, gopp::error err) {
    auto e = make_ptr<url::Error>();
    e->Op = urlErrorOp(method);
    e->URL = u.String();
    e->Err = std::move(err);
    return e;
}

bool idempotent(std::string_view method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

// Errors on a reused connection that the server may have closed while it
// was idle: the request is safe to retry on a new one.
bool staleConnError(const gopp::error& err) {
    if (err == io::EOF_ || err == io::ErrUnexpectedEOF) return true;
    if (auto* e = errors::As<net::Errno>(err)) return e->Code == ECONNRESET || e->Code == EPIPE;
    return false;
}

gopp::error writeRequest(persistConn& pc, Request& req, bool close) {
    std::string out;
    out.reserve(256);
    out.append(req.Method.empty() ? std::string_view("GET") : req.Method.view());
    out.push_back(' ');
    out.append(req.URL.RequestURI().view());
    out.append(" HTTP/1.1\r\nHost: ");
    out.append(req.Host.empty() ? req.URL.Host.view() : req.Host.view());
    out.append("\r\n");
    // The caller's User-Agent, or the default; none if the caller's is
    // empty, as in Go.
    if (!req.Header.Has("User-Agent")) {
        out.append("User-Agent: gopp-http-client/1.1\r\n");
    } else if (string ua = req.Header.Get("User-Agent"); !ua.empty()) {
        out.append("User-Agent: ");
        out.append(ua.view());
        out.append("\r\n");
    }
    if (close && !req.Header.Has("Connection")) out.append("Connection: close\r\n");
    bool chunked = false;
    if (req.Body) {
        if (req.ContentLength > 0) {
            out.append("Content-Length: ");
            out.append(strconv::Itoa(req.ContentLength).view());
            out.append("\r\n");
        } else if (req.ContentLength < 0) {
            out.append("Transfer-Encoding: chunked\r\n");
            chunked = true;
        }
    }
    req.Header.WriteTo(out, {"Host", "User-Agent", "Content-Length", "Transfer-Encoding", "Trailer"});
    out.append("\r\n");
    if (auto [n, err] = pc.bw.WriteString(out); err) return err;

    if (req.Body && req.ContentLength != 0) {
        byte buf[16 * 1024];
        for (;;) {
            auto [n, rerr] = req.Body->Read(buf);
            if (n > 0) {
                if (chunked) {
                    char hex[20];
                    auto r = std::to_chars(hex, hex + 16, n, 16);
                    *r.ptr++ = '\r';
                    *r.ptr++ = '\n';
                    pc.bw.WriteString(std::string_view(hex, static_cast<std::size_t>(r.ptr - hex)));
                }
                if (auto [m, werr] = pc.bw.Write(std::span<const byte>(buf, static_cast<std::size_t>(n))); werr) return werr;
                if (chunked) pc.bw.WriteString("\r\n");
            }
            if (rerr == io::EOF_) break;
            if (rerr) return rerr;
        }
        if (chunked) pc.bw.WriteString("0\r\n\r\n");
        req.Body->Close();
    }
    return pc.bw.Flush();
}

struct readResult {
    ptr<Response> resp;
    gopp::error err;
};

// Reads a response header, skipping 1xx responses other than 101.
readResult readResponseHeader(persistConn& pc, Request& req) {
    for (;;) {
        auto resp = make_ptr<Response>();
        string line;
        if (gopp::error err = internal::readHeaderBlock(pc.br, 10 << 20, line, resp->Header)) {
            return {nullptr, err};
        }
        if (line.empty()) return {nullptr, errMalformedResponse};
        auto sp = line.find(' ');
        if (sp == string::npos) return {nullptr, errMalformedResponse};
        resp->Proto = line.slice(0, sp);
        resp->Status = strings::TrimLeft(line.slice(sp + 1), " ");
        if (!internal::parseHttpVersion(resp->Proto, resp->ProtoMajor, resp->ProtoMinor)) {
            return {nullptr, errors::New("malformed HTTP version " + strconv::Quote(resp->Proto))};
        }
        std::string_view code = resp->Status.view().substr(0, 3);
        if (code.size() != 3 || (resp->Status.size() > 3 && resp->Status[3] != ' ')) {
            return {nullptr, errors::New("malformed HTTP status code " + strconv::Quote(code))};
        }
        auto [c, cerr] = strconv::Atoi(code);
        if (cerr || c < 100) return {nullptr, errors::New("malformed HTTP status code " + strconv::Quote(code))};
        resp->StatusCode = static_cast<int>(c);
        if (resp->StatusCode >= 100 && resp->StatusCode < 200 && resp->StatusCode != 101) continue;
        return {resp, nullptr};
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Transport

Transport::Transport() : impl_(std::make_shared<impl>()) {}

Transport::~Transport() { impl_->closeIdle(); }

void Transport::CloseIdleConnections() { impl_->closeIdle(); }

namespace {

const gopp::error errReadOnClosedResBody = errors::New("http: read on closed response body");

// Go's gzipReader: decompresses a response body lazily, on the first Read.
// Close may come from another fiber while a Read waits for the body: it
// closes the body, which ends the Read.
class gzipReader final : public io::ReadCloser {
public:
    explicit gzipReader(ptr<io::ReadCloser> body) : body_(std::move(body)) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (closed_.load(std::memory_order_acquire)) return {0, errReadOnClosedResBody};
        if (!zr_ && !zerr_) {
            auto [zr, err] = gzip::NewReader(body_);
            zr_ = std::move(zr);
            zerr_ = err;
        }
        if (zerr_) return {0, zerr_};
        auto [n, err] = zr_->Read(p);
        if (err && closed_.load(std::memory_order_acquire)) return {n, errReadOnClosedResBody};
        return {n, err};
    }

    gopp::error Close() override {
        closed_.store(true, std::memory_order_release);
        return body_->Close();
    }

private:
    ptr<io::ReadCloser> body_;
    ptr<gzip::Reader> zr_;
    gopp::error zerr_;
    std::atomic<bool> closed_{false};
};

}  // namespace

std::pair<ptr<Response>, gopp::error> Transport::RoundTrip(Request& req) {
    // Ask for gzip if the caller didn't ask for an encoding, and decompress
    // it transparently. Range requests aren't, as the ranges would be of
    // the compressed body (see Go's issue 8923).
    bool requestedGzip = !DisableCompression && !req.Header.Has("Accept-Encoding") && !req.Header.Has("Range") &&
                         req.Method != "HEAD";
    if (!requestedGzip) return roundTrip(req);
    // The header is for this round trip only: the caller's request stays as
    // it was, as Go's Transport adds it on the wire.
    req.Header.Set("Accept-Encoding", "gzip");
    auto [resp, err] = roundTrip(req);
    req.Header.Del("Accept-Encoding");
    if (err) return {resp, err};
    int code = resp->StatusCode;
    bool hasBody = code != 204 && code != 304 && !(code >= 100 && code < 200);
    if (hasBody && ascii::EqualFold(resp->Header.Get("Content-Encoding"), "gzip")) {
        resp->Body = make_ptr<gzipReader>(resp->Body);
        resp->Header.Del("Content-Encoding");
        resp->Header.Del("Content-Length");
        resp->ContentLength = -1;
        resp->Uncompressed = true;
    }
    return {resp, nullptr};
}

std::pair<ptr<Response>, gopp::error> Transport::roundTrip(Request& req) {
    bool https = req.URL.Scheme == "https";
    if (req.URL.Scheme != "http" && !https) {
        if (req.URL.Scheme.empty()) return {nullptr, errors::New("unsupported protocol scheme \"\"")};
        return {nullptr, errors::New("unsupported protocol scheme " + strconv::Quote(req.URL.Scheme))};
    }
    if (req.URL.Host.empty()) return {nullptr, errors::New("http: no Host in request URL")};

    std::string addr(req.URL.Host.view());
    if (req.URL.Port().empty()) addr += https ? ":443" : ":80";
    std::string key = std::string(req.URL.Scheme.view()) + "://" + addr;

    // A context deadline becomes the connection's deadline.
    std::optional<time::Time> deadline;
    if (req.ctx_) {
        if (gopp::error err = req.ctx_->Err()) return {nullptr, err};
        deadline = req.ctx_->Deadline();
    }

    // HTTP/2: negotiated with ALPN over TLS, or h2c with prior knowledge
    // when the transport speaks only unencrypted HTTP/2 to http:// URLs.
    http::Protocols protos = Protocols.value_or(DefaultProtocols());
    bool h2c = !https && protos.UnencryptedHTTP2() && !protos.HTTP1();
    bool mayH2 = (https && protos.HTTP2() && internal::http2ClientEnabled) || h2c;
    internal::http2ClientOptions h2opts{.responseHeaderTimeout = ResponseHeaderTimeout};
    // Retries a request the server didn't process (after GOAWAY or on a
    // connection that broke before the request went out).
    auto h2RoundTrip = [&](const ptr<internal::http2ClientConn>& cc, int attempt, bool& retry) {
        auto [resp, err] = cc->roundTrip(req, req.ctx_);
        retry = err && errors::Is(err, internal::errHttp2ConnUnusable) && attempt < 3;
        return std::pair<ptr<Response>, gopp::error>{resp, err};
    };

    for (int attempt = 0;; attempt++) {
        if (mayH2) {
            if (auto cc = impl_->h2Get(key)) {
                bool retry;
                auto res = h2RoundTrip(cc, attempt, retry);
                if (retry) continue;
                return res;
            }
        }
        ptr<persistConn> pc = DisableKeepAlives || h2c ? nullptr : impl_->getIdle(key);
        bool reused = static_cast<bool>(pc);
        if (!pc) {
            time::Duration dt = DialTimeout;
            if (deadline) {
                time::Duration left = time::Until(*deadline);
                if (left.ns <= 0) return {nullptr, context::DeadlineExceeded};
                if (dt.ns == 0 || left < dt) dt = left;
            }
            auto [c, err] = net::DialTimeout("tcp", addr, dt);
            if (err) return {nullptr, err};
            if (https) {
                tls::Config cfg = impl_->clientTlsConfig(*this);
                if (cfg.ServerName.empty()) cfg.ServerName = req.URL.Hostname();
                auto tc = tls::Client(c, cfg);
                if (TLSHandshakeTimeout.ns > 0) c->SetDeadline(time::Now().Add(TLSHandshakeTimeout));
                if (gopp::error herr = tc->Handshake()) {
                    c->Close();
                    return {nullptr, herr};
                }
                c->SetDeadline(time::Time{});
                tls::ConnectionState state = tc->ConnectionState();
                if (state.NegotiatedProtocol == "h2") {
                    auto cc = internal::newHttp2ClientConn(tc, state, h2opts);
                    impl_->h2Put(key, cc);
                    bool retry;
                    auto res = h2RoundTrip(cc, attempt, retry);
                    if (retry) continue;
                    return res;
                }
                pc = make_ptr<persistConn>(key, tc);
                pc->tls = state;
            } else if (h2c) {
                auto cc = internal::newHttp2ClientConn(c, std::nullopt, h2opts);
                impl_->h2Put(key, cc);
                bool retry;
                auto res = h2RoundTrip(cc, attempt, retry);
                if (retry) continue;
                return res;
            } else {
                pc = make_ptr<persistConn>(key, c);
            }
        }
        if (deadline) {
            pc->conn->SetDeadline(*deadline);
            pc->hasDeadline = true;
        }

        gopp::error err = writeRequest(*pc, req, DisableKeepAlives);
        readResult rr;
        if (!err) {
            if (ResponseHeaderTimeout.ns > 0 && !deadline) {
                pc->conn->SetReadDeadline(time::Now().Add(ResponseHeaderTimeout));
            }
            rr = readResponseHeader(*pc, req);
            err = rr.err;
            if (ResponseHeaderTimeout.ns > 0 && !deadline) pc->conn->SetReadDeadline(time::Time{});
        }
        if (err) {
            pc->conn->Close();
            if (reused && attempt == 0 && idempotent(req.Method) && !req.Body && staleConnError(err)) continue;
            if (deadline && err->Timeout()) err = context::DeadlineExceeded;
            return {nullptr, err};
        }

        // Body framing (after Go's transfer.Go readTransfer).
        ptr<Response> resp = rr.resp;
        resp->TLS = pc->tls;
        resp->Close = DisableKeepAlives || req.Close ||
                      (resp->ProtoMajor == 1 && resp->ProtoMinor == 0
                           ? !internal::hasToken(resp->Header.Get("Connection"), "keep-alive")
                           : internal::hasToken(resp->Header.Get("Connection"), "close"));
        bool reusable = !resp->Close;
        int code = resp->StatusCode;
        bool noBody = req.Method == "HEAD" || code == 204 || code == 304 || (code >= 100 && code < 200);

        std::vector<string> te = resp->Header.Values("Transfer-Encoding");
        bool chunked = !te.empty() && ascii::EqualFold(te.back(), "chunked");
        if (chunked) {
            resp->TransferEncoding = {string::fromStatic("chunked")};
            resp->Header.Del("Content-Length");
        }
        string cl = resp->Header.Get("Content-Length");
        if (!cl.empty()) {
            auto [n, perr] = strconv::ParseInt(strings::TrimSpace(cl).view(), 10, 64);
            if (perr || n < 0) {
                pc->conn->Close();
                return {nullptr, errors::New("bad Content-Length " + strconv::Quote(cl))};
            }
            resp->ContentLength = n;
        }
        if (noBody) {
            if (req.Method != "HEAD") resp->ContentLength = 0;
            resp->Body = internal::noBody();
        } else if (chunked) {
            resp->ContentLength = -1;
            resp->Body = make_ptr<clientBody>(impl_, pc, make_ptr<internal::chunkedReader>(pc->br), nullptr, reusable,
                                               MaxIdleConnsPerHost, MaxIdleConns);
            return {resp, nullptr};
        } else if (resp->ContentLength > 0) {
            auto lr = make_ptr<io::LimitedReader>(pc->br, resp->ContentLength);
            io::LimitedReader* raw = lr.get();
            resp->Body = make_ptr<clientBody>(impl_, pc, std::move(lr), raw, reusable, MaxIdleConnsPerHost,
                                               MaxIdleConns);
            return {resp, nullptr};
        } else if (resp->ContentLength == 0) {
            resp->Body = internal::noBody();
        } else {
            // Neither length nor chunking: the body ends when the connection does.
            resp->Close = true;
            resp->Body = make_ptr<clientBody>(impl_, pc, pc->br, nullptr, false, 0, 0);
            return {resp, nullptr};
        }

        // No body to read: the connection is free now.
        if (reusable) {
            if (pc->hasDeadline) {
                pc->conn->SetDeadline(time::Time{});
                pc->hasDeadline = false;
            }
            if (impl_->putIdle(pc, MaxIdleConnsPerHost, MaxIdleConns)) return {resp, nullptr};
        }
        pc->conn->Close();
        return {resp, nullptr};
    }
}

const ptr<Transport>& DefaultTransport() {
    static ptr<Transport>* t = new ptr<Transport>(make_ptr<Transport>());  // never freed
    return *t;
}

// ---------------------------------------------------------------------------
// Requests and the client

std::pair<ptr<Request>, gopp::error> NewRequestWithContext(ptr<context::Context> ctx, std::string_view method,
                                                              std::string_view rawurl, ptr<io::Reader> body) {
    if (method.empty()) method = "GET";
    if (!internal::validMethod(method)) return {nullptr, errors::New("net/http: invalid method " + strconv::Quote(method))};
    if (!ctx) return {nullptr, errors::New("net/http: nil Context")};
    auto [u, err] = url::Parse(rawurl);
    if (err) return {nullptr, err};
    auto req = make_ptr<Request>();
    req->Method = string(method);
    req->Host = u.Host;
    req->URL = std::move(u);
    if (ctx != context::Background()) req->ctx_ = std::move(ctx);
    if (body) {
        if (auto* sr = dynamic_cast<io::StringReader*>(body.get())) {
            req->ContentLength = static_cast<int64>(sr->len());
        } else {
            req->ContentLength = -1;
        }
        if (auto rc = dynamic_pointer_cast<io::ReadCloser>(body)) req->Body = std::move(rc);
        else req->Body = io::NopCloser(std::move(body));
        if (req->ContentLength == 0) req->Body = nullptr;
    }
    return {req, nullptr};
}

std::pair<ptr<Request>, gopp::error> NewRequest(std::string_view method, std::string_view rawurl, ptr<io::Reader> body) {
    return NewRequestWithContext(context::Background(), method, rawurl, std::move(body));
}

namespace {

bool isRedirect(int code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

// Resolves a Location header against the request URL.
std::pair<string, gopp::error> resolveLocation(const url::URL& base, const string& loc) {
    if (loc.find("://") != string::npos) return {loc, nullptr};
    url::URL u = base;
    u.RawPath = {};
    u.Fragment = {};
    std::string_view l = loc;
    std::string_view query;
    if (auto q = l.find('?'); q != l.npos) {
        query = l.substr(q + 1);
        l = l.substr(0, q);
    }
    if (l.starts_with("//")) return {base.Scheme + ":" + loc, nullptr};
    string path;
    if (l.starts_with("/")) {
        path = string(l);
    } else {
        std::string_view dir = base.Path.view().substr(0, base.Path.view().rfind('/') + 1);
        path = internal::cleanPath(string(dir) + string(l));
    }
    auto [p, err] = url::PathUnescape(path);
    if (err) return {{}, err};
    u.Path = p;
    u.RawQuery = string(query);
    return {u.String(), nullptr};
}

// Go's cancelTimerBody: the body of a response to a Client with a Timeout,
// which stops the timer when it's closed.
class cancelTimerBody final : public io::ReadCloser {
public:
    cancelTimerBody(ptr<io::ReadCloser> rc, context::CancelFunc stop) : rc_(std::move(rc)), stop_(std::move(stop)) {}
    std::pair<isize, gopp::error> Read(std::span<byte> p) override { return rc_->Read(p); }
    gopp::error Close() override {
        gopp::error err = rc_->Close();
        stop_();
        return err;
    }

private:
    ptr<io::ReadCloser> rc_;
    context::CancelFunc stop_;
};

}  // namespace

std::pair<ptr<Response>, gopp::error> Client::Do(ptr<Request> req) {
    if (!req) return {nullptr, errors::New("net/http: nil Request")};
    ptr<RoundTripper> rt = Transport ? Transport : ptr<RoundTripper>(DefaultTransport());
    // The timeout covers the redirects and reading the body. Its timer
    // stops when the request fails, or when the body is closed.
    struct timerStopper {
        context::CancelFunc stop;
        ~timerStopper() {
            if (stop) stop();
        }
    } timer;
    if (Timeout.ns > 0) {
        auto [ctx, cancel] = context::WithTimeout(req->Context(), Timeout);
        req = req->WithContext(ctx);
        timer.stop = std::move(cancel);
    }
    auto done = [&](ptr<Response> resp) -> std::pair<ptr<Response>, gopp::error> {
        if (timer.stop) resp->Body = make_ptr<cancelTimerBody>(resp->Body, std::exchange(timer.stop, nullptr));
        return {resp, nullptr};
    };
    for (int redirects = 0;; redirects++) {
        auto [resp, err] = rt->RoundTrip(*req);
        if (err) return {nullptr, urlError(req->Method, req->URL, err)};
        resp->Request = req;
        if (!isRedirect(resp->StatusCode) || MaxRedirects <= 0) return done(resp);
        string loc = resp->Header.Get("Location");
        if (loc.empty()) return done(resp);
        // 307 and 308 resend the body, which only works without one.
        if ((resp->StatusCode == 307 || resp->StatusCode == 308) && req->Body) return done(resp);
        if (redirects + 1 > MaxRedirects) {
            resp->Body->Close();
            return {nullptr, urlError(req->Method, req->URL, errors::New("stopped after " + strconv::Itoa(MaxRedirects) + " redirects"))};
        }
        auto [target, rerr] = resolveLocation(req->URL, loc);
        if (rerr) {
            resp->Body->Close();
            return {nullptr, urlError(req->Method, req->URL, rerr)};
        }
        // Drain a little of the body so that the connection can be reused.
        io::CopyN(io::Discard(), *resp->Body, 2048);
        resp->Body->Close();

        string method = req->Method;
        if ((resp->StatusCode == 301 || resp->StatusCode == 302 || resp->StatusCode == 303) && method != "HEAD") {
            method = string::fromStatic("GET");
        }
        auto [next, nerr] = NewRequestWithContext(req->Context(), method, target);
        if (nerr) return {nullptr, urlError(req->Method, req->URL, nerr)};
        for (auto& e : req->Header) next->Header.AddCanonical(e.key, e.value);
        req = next;
    }
}

std::pair<ptr<Response>, gopp::error> Client::Get(std::string_view url) {
    auto [req, err] = NewRequest("GET", url);
    if (err) return {nullptr, err};
    return Do(req);
}

std::pair<ptr<Response>, gopp::error> Client::Head(std::string_view url) {
    auto [req, err] = NewRequest("HEAD", url);
    if (err) return {nullptr, err};
    return Do(req);
}

std::pair<ptr<Response>, gopp::error> Client::Post(std::string_view url, std::string_view contentType, ptr<io::Reader> body) {
    auto [req, err] = NewRequest("POST", url, std::move(body));
    if (err) return {nullptr, err};
    req->Header.Set("Content-Type", string(contentType));
    return Do(req);
}

std::pair<ptr<Response>, gopp::error> Client::PostForm(std::string_view url, const url::Values& data) {
    return Post(url, "application/x-www-form-urlencoded", strings::NewReader(data.Encode()));
}

void Client::CloseIdleConnections() {
    ptr<RoundTripper> rt = Transport ? Transport : ptr<RoundTripper>(DefaultTransport());
    if (auto t = dynamic_pointer_cast<http::Transport>(rt)) t->CloseIdleConnections();
}

Client& DefaultClient() {
    static Client* c = new Client();  // never freed
    return *c;
}

std::pair<ptr<Response>, gopp::error> Get(std::string_view url) { return DefaultClient().Get(url); }
std::pair<ptr<Response>, gopp::error> Head(std::string_view url) { return DefaultClient().Head(url); }
std::pair<ptr<Response>, gopp::error> Post(std::string_view url, std::string_view contentType, ptr<io::Reader> body) {
    return DefaultClient().Post(url, contentType, std::move(body));
}
std::pair<ptr<Response>, gopp::error> PostForm(std::string_view url, const url::Values& data) {
    return DefaultClient().PostForm(url, data);
}

}  // namespace gopp::http
