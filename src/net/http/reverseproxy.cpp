// httputil.ReverseProxy, ported from Go 1.27.3's
// net/http/httputil/reverseproxy.go.
#include "ascii.hpp"
#include "internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/mime.hpp>
#include <gopp/net.hpp>
#include <gopp/net/http/httputil.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <mutex>

namespace gopp::httputil {

namespace {

std::string singleJoiningSlash(std::string_view a, std::string_view b) {
    bool aslash = a.ends_with('/');
    bool bslash = b.starts_with('/');
    if (aslash && bslash) return std::string(a) + std::string(b.substr(1));
    if (!aslash && !bslash) return std::string(a) + "/" + std::string(b);
    return std::string(a) + std::string(b);
}

std::pair<string, string> joinURLPath(const url::URL& a, const url::URL& b) {
    if (a.RawPath.empty() && b.RawPath.empty()) return {string(singleJoiningSlash(a.Path, b.Path)), {}};
    // Same as singleJoiningSlash, but uses EscapedPath to determine whether
    // a slash should be added.
    string apath = a.EscapedPath();
    string bpath = b.EscapedPath();
    bool aslash = apath.ends_with("/");
    bool bslash = bpath.starts_with("/");
    if (aslash && bslash) return {a.Path + b.Path.slice(1), apath + bpath.slice(1)};
    if (!aslash && !bslash) return {a.Path + "/" + b.Path, apath + "/" + bpath};
    return {a.Path + b.Path, apath + bpath};
}

void rewriteRequestURL(http::Request& req, const url::URL& target) {
    const string& targetQuery = target.RawQuery;
    req.URL.Scheme = target.Scheme;
    req.URL.Host = target.Host;
    std::tie(req.URL.Path, req.URL.RawPath) = joinURLPath(target, req.URL);
    if (targetQuery.empty() || req.URL.RawQuery.empty()) {
        req.URL.RawQuery = targetQuery + req.URL.RawQuery;
    } else {
        req.URL.RawQuery = targetQuery + "&" + req.URL.RawQuery;
    }
}

void copyHeader(http::Header& dst, const http::Header& src) {
    for (const auto& e : src) dst.Add(e.key, e.value);
}

// Hop-by-hop headers. These are removed when sent to the backend. As of RFC
// 7230, hop-by-hop headers are required to appear in the Connection header
// field. These are the headers defined by the obsoleted RFC 2616 (section
// 13.5.1) and are used for backward compatibility.
constexpr std::string_view hopHeaders[] = {
    "Connection",
    "Proxy-Connection",  // non-standard but still sent by libcurl and rejected by e.g. google
    "Keep-Alive",
    "Proxy-Authenticate",
    "Proxy-Authorization",
    "Te",       // canonicalized version of "TE"
    "Trailer",  // not Trailers per URL above; https://www.rfc-editor.org/errata_search.php?eid=4522
    "Transfer-Encoding",
    "Upgrade",
};

// removeHopByHopHeaders removes hop-by-hop headers.
void removeHopByHopHeaders(http::Header& h) {
    // RFC 7230, section 6.1: Remove headers listed in the "Connection"
    // header.
    for (const auto& f : h.Values("Connection")) {
        for (auto sf : strings::Split(f, ",")) {
            if (sf = strings::TrimSpace(sf); !sf.empty()) h.Del(sf);
        }
    }
    // RFC 2616, section 13.5.1: Remove a set of known hop-by-hop headers.
    // This behavior is superseded by the RFC 7230 Connection header, but
    // preserve it for backwards compatibility.
    for (auto f : hopHeaders) h.Del(f);
}

bool headerValuesContainsToken(const std::vector<string>& values, std::string_view token) {
    for (const auto& v : values) {
        if (http::internal::hasToken(v, token)) return true;
    }
    return false;
}

string upgradeType(const http::Header& h) {
    if (!headerValuesContainsToken(h.Values("Connection"), "Upgrade")) return {};
    return h.Get("Upgrade");
}

bool ishex(char c) { return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F'); }

// cleanQueryParams removes the query parameters that url.ParseQuery would
// drop (those with semicolons or invalid escapes), so that the backend
// sees what the proxy does.
string cleanQueryParams(const string& s) {
    auto reencode = [](const string& s) { return url::ParseQuery(s).first.Encode(); };
    for (std::size_t i = 0; i < s.size();) {
        switch (s[i]) {
        case ';':
            return reencode(s);
        case '%':
            if (i + 2 >= s.size() || !ishex(s[i + 1]) || !ishex(s[i + 2])) return reencode(s);
            i += 3;
            break;
        default:
            i++;
        }
    }
    return s;
}

// A writer that flushes its destination at most latency after a write
// (after every write if latency is negative).
class maxLatencyWriter final : public io::Writer {
public:
    maxLatencyWriter(http::ResponseWriter& dst, http::Flusher* flusher, time::Duration latency)
        : dst_(dst), flusher_(flusher), latency_(latency) {}

    std::pair<isize, error> Write(std::span<const byte> p) override {
        std::lock_guard l(state_->mu);
        auto [n, err] = dst_.Write(p);
        if (latency_.ns < 0) {
            flush();
            return {n, err};
        }
        if (state_->flushPending) return {n, err};
        auto st = state_;
        auto self = this;
        if (!state_->t) {
            state_->t = time::AfterFunc(latency_, [st, self] { self->delayedFlush(st); });
        } else {
            state_->t->Reset(latency_);
        }
        state_->flushPending = true;
        return {n, err};
    }

    // Stops flushing; the timer may still run, and then does nothing.
    void stop() {
        std::lock_guard l(state_->mu);
        state_->flushPending = false;
        state_->stopped = true;
        if (state_->t) state_->t->Stop();
    }

private:
    struct state {
        sync::Mutex mu;  // protects t, flushPending and the flushes
        ptr<time::Timer> t;
        bool flushPending = false;
        bool stopped = false;
    };

    void delayedFlush(const std::shared_ptr<state>& st) {
        std::lock_guard l(st->mu);
        // If stop was called but AfterFunc already started this goroutine.
        if (!st->flushPending || st->stopped) return;
        flush();
        st->flushPending = false;
    }

    void flush() {
        if (flusher_) flusher_->Flush();
    }

    http::ResponseWriter& dst_;
    http::Flusher* flusher_;
    time::Duration latency_;
    std::shared_ptr<state> state_ = std::make_shared<state>();
};

}  // namespace

// ---------------------------------------------------------------------------
// ProxyRequest

void ProxyRequest::SetURL(const url::URL& target) {
    rewriteRequestURL(*Out, target);
    Out->Host = {};
}

void ProxyRequest::SetXForwarded() {
    auto [clientIP, _, err] = net::SplitHostPort(In->RemoteAddr);
    if (!err) {
        auto prior = Out->Header.Values("X-Forwarded-For");
        if (!prior.empty()) clientIP = strings::Join(prior, ", ") + ", " + clientIP;
        Out->Header.Set("X-Forwarded-For", clientIP);
    } else {
        Out->Header.Del("X-Forwarded-For");
    }
    Out->Header.Set("X-Forwarded-Host", In->Host);
    Out->Header.Set("X-Forwarded-Proto", string::fromStatic(In->TLS ? "https" : "http"));
}

// ---------------------------------------------------------------------------
// ReverseProxy

ptr<ReverseProxy> NewSingleHostReverseProxy(const url::URL& target) {
    auto p = make_ptr<ReverseProxy>();
    p->Director = [target](http::Request& req) { rewriteRequestURL(req, target); };
    return p;
}

template <typename... A>
void ReverseProxy::logf(std::format_string<A...> f, A&&... a) {
    string msg(std::format(f, std::forward<A>(a)...));
    if (ErrorLog) {
        ErrorLog->Print("{}", msg);
    } else {
        log::Print("{}", msg);
    }
}

void ReverseProxy::handleError(http::ResponseWriter& rw, http::Request& req, gopp::error err) {
    if (ErrorHandler) {
        ErrorHandler(rw, req, std::move(err));
        return;
    }
    logf("http: proxy error: {}", err);
    rw.WriteHeader(http::StatusBadGateway);
}

// modifyResponse conditionally runs the optional ModifyResponse hook and
// reports whether the request should proceed.
bool ReverseProxy::modifyResponse(http::ResponseWriter& rw, http::Response& res, http::Request& req) {
    if (!ModifyResponse) return true;
    if (auto err = ModifyResponse(res)) {
        res.Body->Close();
        handleError(rw, req, err);
        return false;
    }
    return true;
}

void ReverseProxy::ServeHTTP(http::ResponseWriter& rw, http::Request& req) {
    ptr<http::RoundTripper> transport = Transport ? Transport : ptr<http::RoundTripper>(http::DefaultTransport());

    // The outgoing request: a copy of the incoming one, with its context.
    auto outreq = make_ptr<http::Request>(req);
    if (req.ContentLength == 0) outreq->Body = nullptr;  // Go's issue 16036: nil Body for http.Transport retries

    if (bool(Director) == bool(Rewrite)) {
        handleError(rw, req, errors::New("ReverseProxy must have exactly one of Director or Rewrite set"));
        return;
    }

    if (Director) {
        Director(*outreq);
        if (outreq->formParsed_) outreq->URL.RawQuery = cleanQueryParams(outreq->URL.RawQuery);
    }
    outreq->Close = false;

    string reqUpType = upgradeType(outreq->Header);
    if (!http::ascii::IsPrint(reqUpType)) {
        handleError(rw, req, fmt::Errorf("client tried to switch to invalid protocol {}", strconv::Quote(reqUpType)));
        return;
    }
    removeHopByHopHeaders(outreq->Header);

    // Issue 21096: tell backend applications that care about trailer
    // support that we support trailers. (We do, but we don't go out of our
    // way to advertise that unless the incoming client request thought it
    // was worth mentioning.) Note that we look at req.Header, not
    // outreq.Header, since the latter has passed through
    // removeHopByHopHeaders.
    if (headerValuesContainsToken(req.Header.Values("Te"), "trailers")) outreq->Header.Set("Te", "trailers");

    // After stripping all the hop-by-hop connection headers above, add back
    // any necessary for protocol upgrades, such as for websockets.
    if (!reqUpType.empty()) {
        outreq->Header.Set("Connection", "Upgrade");
        outreq->Header.Set("Upgrade", reqUpType);
    }

    if (Rewrite) {
        // Strip client-provided forwarding headers. The Rewrite func may
        // use SetXForwarded to set new values for these or copy the
        // previous values from the inbound request.
        outreq->Header.Del("Forwarded");
        outreq->Header.Del("X-Forwarded-For");
        outreq->Header.Del("X-Forwarded-Host");
        outreq->Header.Del("X-Forwarded-Proto");
        // Remove unparsable query parameters from the outbound request.
        outreq->URL.RawQuery = cleanQueryParams(outreq->URL.RawQuery);
        ProxyRequest pr{&req, outreq};
        Rewrite(pr);
        outreq = pr.Out;
    } else if (auto [clientIP, _, err] = net::SplitHostPort(req.RemoteAddr); !err) {
        // If we aren't the first proxy retain prior X-Forwarded-For
        // information as a comma+space separated list and fold multiple
        // headers into one.
        auto prior = outreq->Header.Values("X-Forwarded-For");
        if (!prior.empty()) clientIP = strings::Join(prior, ", ") + ", " + clientIP;
        outreq->Header.Set("X-Forwarded-For", clientIP);
    }

    if (outreq->Method == "CONNECT") {
        // ReverseProxy does not support CONNECT. See Go's issue 25423.
        auto err = errors::New("client sent unsupported CONNECT request");
        if (ErrorHandler) {
            ErrorHandler(rw, *outreq, err);
        } else {
            logf("http: proxy error: {}", err);
            rw.WriteHeader(http::StatusMethodNotAllowed);
        }
        return;
    }

    if (!outreq->Header.Has("User-Agent")) {
        // If the outbound request doesn't have a User-Agent header set,
        // don't send the default one.
        outreq->Header.Set("User-Agent", "");
    }

    auto [res, err] = transport->RoundTrip(*outreq);
    if (err) {
        handleError(rw, *outreq, err);
        return;
    }

    // Deal with 101 Switching Protocols responses: (WebSocket, h2c, etc).
    if (res->StatusCode == http::StatusSwitchingProtocols) {
        if (!modifyResponse(rw, *res, *outreq)) return;
        res->Body->Close();
        handleError(rw, *outreq, errors::New("can't switch protocols using non-Hijacker ResponseWriter"));
        return;
    }

    removeHopByHopHeaders(res->Header);

    if (!modifyResponse(rw, *res, *outreq)) return;

    copyHeader(rw.Header(), res->Header);
    rw.WriteHeader(res->StatusCode);

    err = copyResponse(rw, *res->Body, flushInterval(*res));
    res->Body->Close();
    if (err) {
        // Since we're streaming the response, if we run into an error all we
        // can do is abort the request. Issue 23643: ReverseProxy should use
        // ErrAbortHandler on read error while copying body.
        throw http::ErrAbortHandler;
    }
}

// flushInterval returns the p.FlushInterval value, conditionally overriding
// its value for a specific request/response.
time::Duration ReverseProxy::flushInterval(const http::Response& res) const {
    // For Server-Sent Events responses, flush immediately. The MIME type is
    // defined in https://www.w3.org/TR/eventsource/#text-event-stream
    auto [baseCT, _, _2] = mime::ParseMediaType(res.Header.Get("Content-Type"));
    if (baseCT == "text/event-stream") return time::Duration{-1};  // negative means immediately
    // We might have the case of streaming for which Content-Length might be
    // unset.
    if (res.ContentLength == -1) return time::Duration{-1};
    return FlushInterval;
}

gopp::error ReverseProxy::copyResponse(http::ResponseWriter& dst, io::Reader& src, time::Duration flushInterval) {
    slice<byte> pooled;
    if (BufferPool) pooled = BufferPool->Get();
    byte stackBuf[32 * 1024];
    std::span<byte> buf = pooled.len() > 0 ? pooled.span() : std::span<byte>(stackBuf);

    gopp::error err;
    if (flushInterval.ns != 0) {
        auto mlw = make_ptr<maxLatencyWriter>(dst, dynamic_cast<http::Flusher*>(&dst), flushInterval);
        err = copyBuffer(*mlw, src, buf).second;
        mlw->stop();
    } else {
        err = copyBuffer(dst, src, buf).second;
    }
    if (BufferPool) BufferPool->Put(std::move(pooled));
    return err;
}

// copyBuffer returns any write errors or non-EOF read errors, and the amount
// of bytes written.
std::pair<int64, gopp::error> ReverseProxy::copyBuffer(io::Writer& dst, io::Reader& src, std::span<byte> buf) {
    int64 written = 0;
    for (;;) {
        auto [nr, rerr] = src.Read(buf);
        if (rerr && rerr != io::EOF_ && rerr != context::Canceled) {
            logf("httputil: ReverseProxy read error during body copy: {}", rerr);
        }
        if (nr > 0) {
            auto [nw, werr] = dst.Write(buf.first(std::size_t(nr)));
            if (nw > 0) written += nw;
            if (werr) return {written, werr};
            if (nr != nw) return {written, io::ErrShortWrite};
        }
        if (rerr) {
            if (rerr == io::EOF_) rerr = nullptr;
            return {written, rerr};
        }
    }
}

}  // namespace gopp::httputil
