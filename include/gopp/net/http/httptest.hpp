// The net/http/httptest package: testing HTTP handlers and clients.
//
//     auto rr = httptest::NewRecorder();
//     handler->ServeHTTP(*rr, *httptest::NewRequest("GET", "/items/1"));
//     EXPECT_EQ(rr->Code, 200);
//
//     auto ts = httptest::NewServer(mux);   // on a loopback port
//     auto [resp, err] = http::Get(ts->URL + "/items/1");
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/net/http.hpp>

#include <string>
#endif

namespace gopp::httptest {

// httptest.DefaultRemoteAddr
inline constexpr std::string_view DefaultRemoteAddr = "1.2.3.4";

// httptest.ResponseRecorder: a ResponseWriter that records what the
// handler wrote.
class ResponseRecorder final : public http::ResponseWriter, public http::Flusher {
public:
    // The status code given to WriteHeader; 200 if the handler wrote
    // without calling it.
    int Code = 200;
    // The response body.
    std::string Body;
    // Whether the handler called flush.
    bool Flushed = false;

    http::Header& Header() override { return header_; }
    void WriteHeader(int code) override;
    std::pair<isize, gopp::error> Write(std::span<const byte> p) override;
    void Flush() override;

    // The response as a client would see it: the status, the header as of
    // the first write, and the body (rw.Result()).
    ptr<http::Response> Result();

private:
    http::Header header_;
    http::Header snapHeader_;
    bool wroteHeader_ = false;
};

// httptest.NewRecorder
ptr<ResponseRecorder> NewRecorder();

// httptest.NewRequest: a server request for target, which is a path
// ("/foo") or an absolute URL ("http://example.com/foo"), as a handler
// would receive it. The host is "example.com" unless target names one.
// Panics on an invalid target.
ptr<http::Request> NewRequest(std::string_view method, std::string_view target, ptr<io::Reader> body = nullptr);

// httptest.Server: an HTTP server on a loopback port.
class Server final : public object {
public:
    // "http://127.0.0.1:port", or https for NewTLSServer.
    string URL;
    ptr<net::Listener> Listener;
    http::Server Config;

    // Starts serving HTTP, or HTTPS with a test certificate for 127.0.0.1,
    // ::1 and example.com that Client() trusts, on a server made by
    // NewUnstartedServer. Client() speaks Config.Protocols.
    void Start();
    void StartTLS();
    // Shuts the server down and waits for it.
    void Close();
    // A client for the server, whose idle connections close() closes.
    http::Client& Client() noexcept { return client_; }
    ~Server() override;

private:
    friend ptr<Server> NewServer(ptr<http::Handler> handler);
    friend ptr<Server> NewTLSServer(ptr<http::Handler> handler, std::optional<http::Protocols> protocols);
    friend ptr<Server> NewUnstartedServer(ptr<http::Handler> handler);
    void start(bool tls);
    ptr<http::Transport> transport_;
    http::Client client_;
    chan<gopp::error> done_;
    bool closed_ = false;
};

// httptest.NewServer
ptr<Server> NewServer(ptr<http::Handler> handler);

// httptest.NewUnstartedServer: a server listening on a loopback port,
// which serves once Start or StartTLS is called; change Config first.
ptr<Server> NewUnstartedServer(ptr<http::Handler> handler);

// httptest.NewTLSServer: serves HTTPS with a test certificate for
// 127.0.0.1, ::1 and example.com, which client() trusts. With HTTP/2 on by
// default, as in Go.
ptr<Server> NewTLSServer(ptr<http::Handler> handler, std::optional<http::Protocols> protocols = std::nullopt);

}  // namespace gopp::httptest
