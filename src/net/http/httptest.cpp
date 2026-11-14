// After Go's net/http/httptest.
#include "testcert.hpp"

#include <gopp/net/http/httptest.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

namespace gopp::httptest {

namespace {

bool bodyAllowedForStatus(int status) {
    if (status >= 100 && status <= 199) return false;
    return status != 204 && status != 304;
}

}  // namespace

void ResponseRecorder::WriteHeader(int c) {
    if (c < 100 || c > 999) panic("invalid WriteHeader code " + std::to_string(c));
    if (wroteHeader_) return;
    Code = c;
    wroteHeader_ = true;
    snapHeader_ = header_;
}

std::pair<isize, gopp::error> ResponseRecorder::Write(std::span<const byte> p) {
    if (!wroteHeader_) {
        // Like the server, sniff a missing Content-Type from the first write.
        if (!header_.Has("Content-Type") && !header_.Has("Transfer-Encoding") && bodyAllowedForStatus(Code)) {
            header_.Set("Content-Type", http::DetectContentType(p));
        }
        WriteHeader(200);
    }
    Body.append(asStringView(p));
    return {static_cast<isize>(p.size()), nullptr};
}

void ResponseRecorder::Flush() {
    if (!wroteHeader_) WriteHeader(200);
    Flushed = true;
}

ptr<http::Response> ResponseRecorder::Result() {
    auto res = make_ptr<http::Response>();
    res->Proto = string::fromStatic("HTTP/1.1");
    res->StatusCode = Code;
    res->Status = strconv::Itoa(Code) + " " + string(http::StatusText(Code));
    res->Header = wroteHeader_ ? snapHeader_ : header_;
    res->Body = io::NopCloser(strings::NewReader(string(Body)));
    res->ContentLength = -1;
    if (string cl = res->Header.Get("Content-Length"); !cl.empty()) {
        auto [n, err] = strconv::ParseInt(cl, 10, 63);
        if (!err) res->ContentLength = n;
    }
    return res;
}

ptr<ResponseRecorder> NewRecorder() { return make_ptr<ResponseRecorder>(); }

ptr<http::Request> NewRequest(std::string_view method, std::string_view target, ptr<io::Reader> body) {
    if (method.empty()) method = "GET";
    auto req = make_ptr<http::Request>();
    req->Method = string(method);
    req->Proto = string::fromStatic("HTTP/1.1");
    req->ProtoMajor = 1;
    req->ProtoMinor = 1;
    req->RequestURI = string(target);
    auto [u, err] = target.starts_with("/") ? url::ParseRequestURI(target) : url::Parse(target);
    if (err) panic("invalid NewRequest arguments; " + std::string(err.Error().view()));
    req->Host = !u.Host.empty() ? u.Host : string::fromStatic("example.com");
    req->URL = std::move(u);
    req->RemoteAddr = string::fromStatic("192.0.2.1:1234");
    if (body) {
        if (auto* sr = dynamic_cast<io::StringReader*>(body.get())) req->ContentLength = static_cast<int64>(sr->len());
        else req->ContentLength = -1;
        if (auto rc = dynamic_pointer_cast<io::ReadCloser>(body)) req->Body = std::move(rc);
        else req->Body = io::NopCloser(std::move(body));
    } else {
        req->Body = io::NopCloser(strings::NewReader(string()));
    }
    return req;
}

void Server::Close() {
    if (closed_) return;
    closed_ = true;
    if (!done_) {
        Listener->Close();  // never started
        return;
    }
    Config.Close();
    done_.recv();
    transport_->CloseIdleConnections();
    // As Go's: also the default transport's, which http::Get used.
    http::DefaultTransport()->CloseIdleConnections();
}

Server::~Server() { Close(); }

namespace {

ptr<net::Listener> localListener() {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    if (err) panic("httptest: failed to listen on a port: " + std::string(err.Error().view()));
    return ln;
}

}  // namespace

void Server::start(bool tls) {
    if (done_) panic("httptest: Server already started");
    ptr<net::Listener> ln = Listener;
    URL = (tls ? "https://" : "http://") + ln->Addr()->String();
    transport_ = make_ptr<http::Transport>();
    client_.Transport = transport_;
    transport_->Protocols = Config.Protocols;
    done_ = make_chan<gopp::error>(1);
    Server* raw = this;
    if (tls) {
        if (!Config.TLSConfig) {
            auto [cert, err] = tls::X509KeyPair(testcert::localhostCert, testcert::localhostKey);
            if (err) panic("httptest: StartTLS: " + std::string(err.Error().view()));
            tls::Config cfg;
            cfg.Certificates = {cert};
            Config.TLSConfig = cfg;
        }
        // Trust the test certificate.
        tls::Config cfg;
        cfg.RootCAs = x509::NewCertPool();
        cfg.RootCAs->AppendCertsFromPEM(testcert::localhostCert);
        transport_->TLSClientConfig = cfg;
        go([raw, ln] { raw->done_.send(raw->Config.ServeTLS(ln, "", "")); });
    } else {
        go([raw, ln] { raw->done_.send(raw->Config.Serve(ln)); });
    }
}

void Server::Start() { start(false); }
void Server::StartTLS() { start(true); }

ptr<Server> NewUnstartedServer(ptr<http::Handler> handler) {
    auto s = make_ptr<Server>();
    s->Config.Handler = std::move(handler);
    s->Listener = localListener();
    return s;
}

ptr<Server> NewServer(ptr<http::Handler> handler) {
    auto s = NewUnstartedServer(std::move(handler));
    s->Start();
    return s;
}

ptr<Server> NewTLSServer(ptr<http::Handler> handler, std::optional<http::Protocols> protocols) {
    auto s = NewUnstartedServer(std::move(handler));
    s->Config.Protocols = protocols;
    s->StartTLS();
    return s;
}

}  // namespace gopp::httptest
