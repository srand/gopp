// The net/http/httputil package: ReverseProxy, ported from Go 1.27.3.
//
//     auto [target, err] = url::Parse("http://localhost:9000");
//     auto proxy = httputil::NewSingleHostReverseProxy(target);
//     http::ListenAndServe(":8080", proxy);
//
//     // Or with Rewrite, which sets X-Forwarded-* and the outgoing Host:
//     auto proxy = make_ptr<httputil::ReverseProxy>();
//     proxy->Rewrite = [target](httputil::ProxyRequest& r) {
//         r.SetURL(target);
//         r.SetXForwarded();
//     };
//
// Protocol upgrades (101 Switching Protocols, as WebSocket uses) aren't
// proxied yet: go++'s server has no Hijacker.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/log.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/url.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/time.hpp>

#include <functional>
#endif

namespace gopp::httputil {

// httputil.ProxyRequest: the request a ReverseProxy's Rewrite rewrites.
struct ProxyRequest {
    // The request the proxy received. Don't change it.
    http::Request* In = nullptr;
    // The request the proxy will send, which Rewrite may change or replace.
    // Its hop-by-hop headers and X-Forwarded-* are removed already.
    ptr<http::Request> Out;

    // Routes Out to target: its scheme and host, and its path and query
    // joined before Out's ("/api" + "/users" is "/api/users"). It also
    // empties Out's Host, so that the Host header is target's.
    void SetURL(const url::URL& target);
    // Sets Out's X-Forwarded-For (In's client address, after those of Out's
    // X-Forwarded-For), X-Forwarded-Host (In's Host) and X-Forwarded-Proto
    // ("http" or "https").
    void SetXForwarded();
};

// httputil.BufferPool: buffers for copying response bodies.
struct BufferPool : public virtual object {
    virtual slice<byte> Get() = 0;
    virtual void Put(slice<byte> b) = 0;
};

// httputil.ReverseProxy: a handler that sends each request on to another
// server and copies its response back to the client. Set exactly one of
// Rewrite and Director.
class ReverseProxy final : public http::Handler {
public:
    // Rewrites the outgoing request (Out) from the incoming one (In).
    std::function<void(ProxyRequest&)> Rewrite;
    // Go's older hook: changes the outgoing request, a copy of the incoming
    // one, in place. With Director, X-Forwarded-For is appended to, and the
    // hop-by-hop headers are removed after it.
    std::function<void(http::Request&)> Director;
    // The transport of the outgoing requests; DefaultTransport() if nil.
    ptr<http::RoundTripper> Transport;
    // How often to flush the response body while it's copied: 0 never
    // (until the end), negative after every write. Bodies of unknown
    // length (streaming) and text/event-stream are flushed after every
    // write regardless.
    time::Duration FlushInterval{};
    // The logger of errors; log's default logger if nil.
    ptr<log::Logger> ErrorLog;
    // Buffers for copying bodies; 32 KiB of a fiber's stack if nil.
    ptr<httputil::BufferPool> BufferPool;
    // Changes the backend's response, or returns an error, which goes to
    // ErrorHandler.
    std::function<gopp::error(http::Response&)> ModifyResponse;
    // Handles an error of the backend or of ModifyResponse; by default it
    // logs the error and replies 502 Bad Gateway.
    std::function<void(http::ResponseWriter&, http::Request&, gopp::error)> ErrorHandler;

    void ServeHTTP(http::ResponseWriter& rw, http::Request& req) override;

private:
    template <typename... A>
    void logf(std::format_string<A...> f, A&&... a);
    void handleError(http::ResponseWriter& rw, http::Request& req, gopp::error err);
    bool modifyResponse(http::ResponseWriter& rw, http::Response& res, http::Request& req);
    time::Duration flushInterval(const http::Response& res) const;
    gopp::error copyResponse(http::ResponseWriter& dst, io::Reader& src, time::Duration flushInterval);
    std::pair<int64, gopp::error> copyBuffer(io::Writer& dst, io::Reader& src, std::span<byte> buf);
};

// httputil.NewSingleHostReverseProxy: a ReverseProxy to target, which
// joins target's path and query before the request's. The Host header is
// the incoming request's; use Rewrite and SetURL to send target's.
ptr<ReverseProxy> NewSingleHostReverseProxy(const url::URL& target);

}  // namespace gopp::httputil
