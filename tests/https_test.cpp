#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

using namespace gopp;

namespace {

ptr<http::ServeMux> tlsMux() {
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /info", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "{} tls={} alpn={} addr={}", r.Proto, r.TLS.has_value(),
                   r.TLS ? r.TLS->NegotiatedProtocol : string(), r.RemoteAddr);
    });
    mux->HandleFunc("POST /echo", [](http::ResponseWriter& w, http::Request& r) {
        auto [data, err] = io::ReadAll(*r.Body);
        fmt::Print(w, "{}", asStringView(data));
    });
    return mux;
}

std::string body(const ptr<http::Response>& resp) {
    auto [data, err] = io::ReadAll(*resp->Body);
    EXPECT_FALSE(err) << err;
    resp->Body->Close();
    return std::string(asStringView(data));
}

http::Protocols http1Only() {
    http::Protocols p;
    p.SetHTTP1(true);
    return p;
}

}  // namespace

TEST(Https, GetOverTLS) {
    auto ts = httptest::NewTLSServer(tlsMux(), http1Only());
    EXPECT_TRUE(ts->URL.starts_with("https://127.0.0.1:"));
    auto [resp, err] = ts->Client().Get(std::string(ts->URL.view()) + "/info");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->StatusCode, 200);
    ASSERT_TRUE(resp->TLS);
    EXPECT_EQ(resp->TLS->Version, tls::VersionTLS13);
    EXPECT_EQ(resp->TLS->NegotiatedProtocol, "http/1.1");
    std::string b = body(resp);
    EXPECT_TRUE(b.starts_with("HTTP/1.1 tls=true alpn=http/1.1")) << b;
}

TEST(Https, KeepAliveAndPost) {
    auto ts = httptest::NewTLSServer(tlsMux(), http1Only());
    std::string first;
    for (int i = 0; i < 3; i++) {
        auto [resp, err] = ts->Client().Get(std::string(ts->URL.view()) + "/info");
        ASSERT_FALSE(err) << err;
        std::string b = body(resp);
        std::string addr = b.substr(b.find("addr="));
        if (i == 0) first = addr;
        EXPECT_EQ(addr, first) << "request " << i << " used a new connection";
    }
    auto [resp, err] = ts->Client().Post(std::string(ts->URL.view()) + "/echo", "text/plain", strings::NewReader("secret"));
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(body(resp), "secret");
}

TEST(Https, UntrustedCertificate) {
    auto ts = httptest::NewTLSServer(tlsMux(), http1Only());
    // The default client uses the system roots.
    auto [resp, err] = http::Get(std::string(ts->URL.view()) + "/info");
    ASSERT_TRUE(err);
    EXPECT_TRUE(strings::Contains(err.Error(), "failed to verify certificate")) << err;
}
