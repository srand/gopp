// The HTTP client, after Go's net/http/client.Go and transport.Go.
//
// Go's Transport runs a read and a write goroutine per connection; here a
// round trip writes the request and reads the response on the calling
// fiber, which parks while the network is busy, and the response body
// returns the connection to the pool once it is read to the end.
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <cerrno>
#include <charconv>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace gopp::http {

namespace detail {

class persist_conn final : public object {
public:
    persist_conn(std::string key, ptr<net::Conn> c)
        : key(std::move(key)), conn(std::move(c)), br(make_ptr<bufio::Reader>(conn, 4096)), bw(conn, 4096) {}

    std::string key;
    ptr<net::Conn> conn;
    ptr<bufio::Reader> br;
    bufio::Writer bw;
    bool has_deadline = false;
    std::optional<tls::ConnectionState> tls;
};

}  // namespace detail

using detail::persist_conn;

struct Transport::impl {
    // The TLS config for https, compiled once and shared by connections.
    std::mutex tls_mu;
    std::optional<tls::Config> TLSConfig;

    tls::Config client_tls_config(const Transport& t) {
        std::lock_guard l(tls_mu);
        if (!TLSConfig) {
            TLSConfig = t.TLSClientConfig.value_or(tls::Config{});
            TLSConfig->ctx_ = nullptr;
            if (TLSConfig->NextProtos.empty()) {
                http::Protocols p = t.Protocols.value_or(DefaultProtocols());
                if (p.HTTP2() && internal::http2_client_enabled) TLSConfig->NextProtos.push_back(string::from_static("h2"));
                if (p.HTTP1()) TLSConfig->NextProtos.push_back(string::from_static("http/1.1"));
            }
        }
        return *TLSConfig;
    }

    std::mutex mu;
    std::unordered_map<std::string, std::vector<ptr<persist_conn>>> idle;
    int nidle = 0;

    ptr<persist_conn> get_idle(const std::string& key) {
        std::lock_guard l(mu);
        auto it = idle.find(key);
        if (it == idle.end() || it->second.empty()) return nullptr;
        ptr<persist_conn> pc = std::move(it->second.back());  // most recently used
        it->second.pop_back();
        nidle--;
        return pc;
    }

    // Returns false if the pool is full; the caller closes pc then.
    bool put_idle(const ptr<persist_conn>& pc, int max_per_host, int max_total) {
        std::lock_guard l(mu);
        auto& list = idle[pc->key];
        if (max_per_host <= 0) max_per_host = 2;
        if (static_cast<int>(list.size()) >= max_per_host) return false;
        if (max_total > 0 && nidle >= max_total) return false;
        list.push_back(pc);
        nidle++;
        return true;
    }

    void close_idle() {
        std::unordered_map<std::string, std::vector<ptr<persist_conn>>> old;
        {
            std::lock_guard l(mu);
            old.swap(idle);
            nidle = 0;
        }
        for (auto& [key, list] : old) {
            for (auto& pc : list) pc->conn->Close();
        }
    }
};

namespace {

const gopp::error err_malformed_response = errors::New("malformed HTTP response");

// The body of a response. Reading it to the end returns the connection to
// the pool; closing it early closes the connection, as in Go.
class client_body final : public io::ReadCloser {
public:
    client_body(std::shared_ptr<Transport::impl> t, ptr<persist_conn> pc, ptr<io::Reader> src, io::LimitedReader* lr,
                bool reusable, int max_per_host, int max_total)
        : t_(std::move(t)),
          pc_(std::move(pc)),
          src_(std::move(src)),
          lr_(lr),
          reusable_(reusable),
          max_per_host_(max_per_host),
          max_total_(max_total) {}

    ~client_body() override {
        if (!done_) finish(false);
    }

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (done_) return {0, closed_ ? ErrBodyReadAfterClose : io::EOF_};
        auto [n, err] = src_->Read(p);
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
        closed_ = true;
        if (!done_) finish(false);
        return nullptr;
    }

private:
    void finish(bool reuse) {
        done_ = true;
        ptr<persist_conn> pc = std::move(pc_);
        src_.reset();
        if (!pc) return;
        if (reuse && reusable_) {
            if (pc->has_deadline) {
                pc->conn->SetDeadline(time::Time{});
                pc->has_deadline = false;
            }
            if (t_->put_idle(pc, max_per_host_, max_total_)) return;
        }
        pc->conn->Close();
    }

    std::shared_ptr<Transport::impl> t_;
    ptr<persist_conn> pc_;
    ptr<io::Reader> src_;
    io::LimitedReader* lr_;
    bool reusable_;
    int max_per_host_;
    int max_total_;
    bool done_ = false;
    bool closed_ = false;
};

string url_error_op(std::string_view method) {
    // "GET" -> "Get", as Go names client errors.
    if (method.empty()) return string::from_static("Get");
    return string(method.substr(0, 1)) + strings::ToLower(method.substr(1));
}

gopp::error url_error(std::string_view method, const url::URL& u, gopp::error err) {
    auto e = make_ptr<url::Error>();
    e->Op = url_error_op(method);
    e->URL = u.String();
    e->Err = std::move(err);
    return e;
}

bool idempotent(std::string_view method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

// Errors on a reused connection that the server may have closed while it
// was idle: the request is safe to retry on a new one.
bool stale_conn_error(const gopp::error& err) {
    if (err == io::EOF_ || err == io::ErrUnexpectedEOF) return true;
    if (auto* e = errors::As<net::Errno>(err)) return e->Code == ECONNRESET || e->Code == EPIPE;
    return false;
}

gopp::error write_request(persist_conn& pc, Request& req, bool close) {
    std::string out;
    out.reserve(256);
    out.append(req.Method.empty() ? std::string_view("GET") : req.Method.view());
    out.push_back(' ');
    out.append(req.URL.RequestURI().view());
    out.append(" HTTP/1.1\r\nHost: ");
    out.append(req.Host.empty() ? req.URL.Host.view() : req.Host.view());
    out.append("\r\n");
    if (!req.Header.Has("User-Agent")) out.append("User-Agent: gopp-http-client/1.1\r\n");
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

struct read_result {
    ptr<Response> resp;
    gopp::error err;
};

// Reads a response header, skipping 1xx responses other than 101.
read_result read_response_header(persist_conn& pc, Request& req) {
    for (;;) {
        auto resp = make_ptr<Response>();
        string line;
        if (gopp::error err = internal::read_header_block(pc.br, 10 << 20, line, resp->Header)) {
            return {nullptr, err};
        }
        if (line.empty()) return {nullptr, err_malformed_response};
        auto sp = line.find(' ');
        if (sp == string::npos) return {nullptr, err_malformed_response};
        resp->Proto = line.slice(0, sp);
        resp->Status = strings::TrimLeft(line.slice(sp + 1), " ");
        if (!internal::parse_http_version(resp->Proto, resp->ProtoMajor, resp->ProtoMinor)) {
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

Transport::~Transport() { impl_->close_idle(); }

void Transport::CloseIdleConnections() { impl_->close_idle(); }

std::pair<ptr<Response>, gopp::error> Transport::RoundTrip(Request& req) {
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

    for (int attempt = 0;; attempt++) {
        ptr<persist_conn> pc = DisableKeepAlives ? nullptr : impl_->get_idle(key);
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
                tls::Config cfg = impl_->client_tls_config(*this);
                if (cfg.ServerName.empty()) cfg.ServerName = req.URL.Hostname();
                auto tc = tls::Client(c, cfg);
                if (TLSHandshakeTimeout.ns > 0) c->SetDeadline(time::Now().Add(TLSHandshakeTimeout));
                if (gopp::error herr = tc->Handshake()) {
                    c->Close();
                    return {nullptr, herr};
                }
                c->SetDeadline(time::Time{});
                pc = make_ptr<persist_conn>(key, tc);
                pc->tls = tc->ConnectionState();
            } else {
                pc = make_ptr<persist_conn>(key, c);
            }
        }
        if (deadline) {
            pc->conn->SetDeadline(*deadline);
            pc->has_deadline = true;
        }

        gopp::error err = write_request(*pc, req, DisableKeepAlives);
        read_result rr;
        if (!err) {
            if (ResponseHeaderTimeout.ns > 0 && !deadline) {
                pc->conn->SetReadDeadline(time::Now().Add(ResponseHeaderTimeout));
            }
            rr = read_response_header(*pc, req);
            err = rr.err;
            if (ResponseHeaderTimeout.ns > 0 && !deadline) pc->conn->SetReadDeadline(time::Time{});
        }
        if (err) {
            pc->conn->Close();
            if (reused && attempt == 0 && idempotent(req.Method) && !req.Body && stale_conn_error(err)) continue;
            if (deadline && err->Timeout()) err = context::DeadlineExceeded;
            return {nullptr, err};
        }

        // Body framing (after Go's transfer.Go readTransfer).
        ptr<Response> resp = rr.resp;
        resp->TLS = pc->tls;
        resp->Close = DisableKeepAlives || req.Close ||
                      (resp->ProtoMajor == 1 && resp->ProtoMinor == 0
                           ? !internal::has_token(resp->Header.Get("Connection"), "keep-alive")
                           : internal::has_token(resp->Header.Get("Connection"), "close"));
        bool reusable = !resp->Close;
        int code = resp->StatusCode;
        bool no_body = req.Method == "HEAD" || code == 204 || code == 304 || (code >= 100 && code < 200);

        std::vector<string> te = resp->Header.Values("Transfer-Encoding");
        bool chunked = !te.empty() && strings::EqualFold(te.back(), "chunked");
        if (chunked) {
            resp->TransferEncoding = {string::from_static("chunked")};
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
        if (no_body) {
            if (req.Method != "HEAD") resp->ContentLength = 0;
            resp->Body = internal::no_body();
        } else if (chunked) {
            resp->ContentLength = -1;
            resp->Body = make_ptr<client_body>(impl_, pc, make_ptr<internal::chunked_reader>(pc->br), nullptr, reusable,
                                               MaxIdleConnsPerHost, MaxIdleConns);
            return {resp, nullptr};
        } else if (resp->ContentLength > 0) {
            auto lr = make_ptr<io::LimitedReader>(pc->br, resp->ContentLength);
            io::LimitedReader* raw = lr.get();
            resp->Body = make_ptr<client_body>(impl_, pc, std::move(lr), raw, reusable, MaxIdleConnsPerHost,
                                               MaxIdleConns);
            return {resp, nullptr};
        } else if (resp->ContentLength == 0) {
            resp->Body = internal::no_body();
        } else {
            // Neither length nor chunking: the body ends when the connection does.
            resp->Close = true;
            resp->Body = make_ptr<client_body>(impl_, pc, pc->br, nullptr, false, 0, 0);
            return {resp, nullptr};
        }

        // No body to read: the connection is free now.
        if (reusable) {
            if (pc->has_deadline) {
                pc->conn->SetDeadline(time::Time{});
                pc->has_deadline = false;
            }
            if (impl_->put_idle(pc, MaxIdleConnsPerHost, MaxIdleConns)) return {resp, nullptr};
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
    if (!internal::valid_method(method)) return {nullptr, errors::New("net/http: invalid method " + strconv::Quote(method))};
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

bool is_redirect(int code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

// Resolves a Location header against the request URL.
std::pair<string, gopp::error> resolve_location(const url::URL& base, const string& loc) {
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
        path = internal::clean_path(string(dir) + string(l));
    }
    auto [p, err] = url::PathUnescape(path);
    if (err) return {{}, err};
    u.Path = p;
    u.RawQuery = string(query);
    return {u.String(), nullptr};
}

}  // namespace

std::pair<ptr<Response>, gopp::error> Client::Do(ptr<Request> req) {
    if (!req) return {nullptr, errors::New("net/http: nil Request")};
    ptr<RoundTripper> rt = Transport ? Transport : ptr<RoundTripper>(DefaultTransport());
    if (Timeout.ns > 0) {
        auto [ctx, cancel] = context::WithTimeout(req->Context(), Timeout);
        req = req->WithContext(ctx);
    }
    for (int redirects = 0;; redirects++) {
        auto [resp, err] = rt->RoundTrip(*req);
        if (err) return {nullptr, url_error(req->Method, req->URL, err)};
        resp->Request = req;
        if (!is_redirect(resp->StatusCode) || MaxRedirects <= 0) return {resp, nullptr};
        string loc = resp->Header.Get("Location");
        if (loc.empty()) return {resp, nullptr};
        // 307 and 308 resend the body, which only works without one.
        if ((resp->StatusCode == 307 || resp->StatusCode == 308) && req->Body) return {resp, nullptr};
        if (redirects + 1 > MaxRedirects) {
            resp->Body->Close();
            return {nullptr, url_error(req->Method, req->URL, errors::New("stopped after " + strconv::Itoa(MaxRedirects) + " redirects"))};
        }
        auto [target, rerr] = resolve_location(req->URL, loc);
        if (rerr) {
            resp->Body->Close();
            return {nullptr, url_error(req->Method, req->URL, rerr)};
        }
        // Drain a little of the body so that the connection can be reused.
        io::CopyN(io::Discard(), *resp->Body, 2048);
        resp->Body->Close();

        string method = req->Method;
        if ((resp->StatusCode == 301 || resp->StatusCode == 302 || resp->StatusCode == 303) && method != "HEAD") {
            method = string::from_static("GET");
        }
        auto [next, nerr] = NewRequestWithContext(req->Context(), method, target);
        if (nerr) return {nullptr, url_error(req->Method, req->URL, nerr)};
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

std::pair<ptr<Response>, gopp::error> Client::Post(std::string_view url, std::string_view content_type, ptr<io::Reader> body) {
    auto [req, err] = NewRequest("POST", url, std::move(body));
    if (err) return {nullptr, err};
    req->Header.Set("Content-Type", string(content_type));
    return Do(req);
}

Client& DefaultClient() {
    static Client* c = new Client();  // never freed
    return *c;
}

std::pair<ptr<Response>, gopp::error> Get(std::string_view url) { return DefaultClient().Get(url); }
std::pair<ptr<Response>, gopp::error> Head(std::string_view url) { return DefaultClient().Head(url); }
std::pair<ptr<Response>, gopp::error> Post(std::string_view url, std::string_view content_type, ptr<io::Reader> body) {
    return DefaultClient().Post(url, content_type, std::move(body));
}

}  // namespace gopp::http
