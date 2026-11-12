#include "net/http/testcert.hpp"

#include <gopp/chan.hpp>
#include <gopp/crypto/tls.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

using namespace gopp;

namespace {

tls::Config server_config(std::vector<string> protos = {}) {
    auto [cert, err] = tls::X509KeyPair(testcert::localhost_cert, testcert::localhost_key);
    EXPECT_FALSE(err) << err;
    tls::Config cfg;
    cfg.Certificates = {cert};
    cfg.NextProtos = std::move(protos);
    return cfg;
}

tls::Config client_config(std::vector<string> protos = {}) {
    tls::Config cfg;
    cfg.RootCAs = x509::NewCertPool();
    EXPECT_TRUE(cfg.RootCAs->AppendCertsFromPEM(testcert::localhost_cert));
    cfg.NextProtos = std::move(protos);
    return cfg;
}

// A TLS echo server; returns its address.
string echo_server(const tls::Config& cfg, ptr<net::Listener>& out) {
    auto [ln, err] = tls::Listen("tcp", "127.0.0.1:0", cfg);
    EXPECT_FALSE(err) << err;
    out = ln;
    go([ln] {
        for (;;) {
            auto [c, err] = ln->Accept();
            if (err) return;
            go([c] {
                io::Copy(*c, *c);
                c->Close();
            });
        }
    });
    return ln->Addr()->String();
}

}  // namespace

TEST(Tls, KeyPair) {
    auto [cert, err] = tls::X509KeyPair(testcert::localhost_cert, testcert::localhost_key);
    EXPECT_FALSE(err) << err;
    auto [bad, berr] = tls::X509KeyPair("not pem", testcert::localhost_key);
    EXPECT_TRUE(berr);
    auto [nf, nerr] = tls::LoadX509KeyPair("/nonexistent/cert.pem", "/nonexistent/key.pem");
    EXPECT_TRUE(nerr);
}

TEST(Tls, HandshakeAndEcho) {
    ptr<net::Listener> ln;
    string addr = echo_server(server_config(), ln);
    auto [c, err] = tls::Dial("tcp", addr, client_config());
    ASSERT_FALSE(err) << err;
    auto st = c->ConnectionState();
    EXPECT_TRUE(st.HandshakeComplete);
    EXPECT_EQ(st.Version, tls::VersionTLS13);
    EXPECT_FALSE(st.CipherSuite.empty());

    io::WriteString(*c, "hello tls");
    byte buf[64];
    auto [n, rerr] = io::ReadFull(*c, std::span<byte>(buf, 9));
    ASSERT_FALSE(rerr) << rerr;
    EXPECT_EQ(as_string_view(std::span<const byte>(buf, 9)), "hello tls");
    c->Close();
    ln->Close();
}

TEST(Tls, LargeTransferAndConcurrentReadWrite) {
    // One fiber writes 1 MiB while another reads the echo of it.
    ptr<net::Listener> ln;
    string addr = echo_server(server_config(), ln);
    auto [c, err] = tls::Dial("tcp", addr, client_config());
    ASSERT_FALSE(err) << err;
    std::string data(1 << 20, '\0');
    for (std::size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>(i * 7);
    auto written = make_chan<error>(1);
    go([c, &data, written] { written.send(io::WriteString(*c, data).second); });
    std::string got(data.size(), '\0');
    auto [n, rerr] = io::ReadFull(*c, std::span<byte>(reinterpret_cast<byte*>(got.data()), got.size()));
    EXPECT_FALSE(rerr) << rerr;
    EXPECT_FALSE(written.recv());
    EXPECT_TRUE(got == data);
    c->Close();
    ln->Close();
}

TEST(Tls, ALPN) {
    ptr<net::Listener> ln;
    string addr = echo_server(server_config({"h2", "http/1.1"}), ln);
    {
        auto [c, err] = tls::Dial("tcp", addr, client_config({"http/1.1", "h2"}));
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(c->ConnectionState().NegotiatedProtocol, "h2");  // the server's preference
        c->Close();
    }
    {
        auto [c, err] = tls::Dial("tcp", addr, client_config({"http/1.1"}));
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(c->ConnectionState().NegotiatedProtocol, "http/1.1");
        c->Close();
    }
    {
        auto [c, err] = tls::Dial("tcp", addr, client_config({"spdy/3"}));
        ASSERT_FALSE(err) << err;  // no overlap: no protocol
        EXPECT_EQ(c->ConnectionState().NegotiatedProtocol, "");
        c->Close();
    }
    ln->Close();
}

TEST(Tls, Verification) {
    ptr<net::Listener> ln;
    string addr = echo_server(server_config(), ln);

    // The system roots don't trust Go's test certificate.
    auto [c1, e1] = tls::Dial("tcp", addr, tls::Config{});
    ASSERT_TRUE(e1);
    EXPECT_TRUE(strings::Contains(e1.Error(), "failed to verify certificate")) << e1;

    tls::Config skip;
    skip.InsecureSkipVerify = true;
    auto [c2, e2] = tls::Dial("tcp", addr, skip);
    EXPECT_FALSE(e2) << e2;
    if (c2) c2->Close();

    // The certificate is for example.com and *.example.com, not other names.
    tls::Config ok = client_config();
    ok.ServerName = "www.example.com";
    auto [c3, e3] = tls::Dial("tcp", addr, ok);
    EXPECT_FALSE(e3) << e3;
    if (c3) c3->Close();

    tls::Config wrong = client_config();
    wrong.ServerName = "example.org";
    auto [c4, e4] = tls::Dial("tcp", addr, wrong);
    ASSERT_TRUE(e4);
    EXPECT_TRUE(strings::Contains(e4.Error(), "failed to verify certificate")) << e4;
    ln->Close();
}

TEST(Tls, CloseNotifyGivesEOF) {
    auto [ln, err] = tls::Listen("tcp", "127.0.0.1:0", server_config());
    ASSERT_FALSE(err);
    go([ln] {
        auto [c, err] = ln->Accept();
        if (err) return;
        io::WriteString(*c, "bye");
        c->Close();
    });
    auto [c, derr] = tls::Dial("tcp", ln->Addr()->String(), client_config());
    ASSERT_FALSE(derr) << derr;
    auto [data, rerr] = io::ReadAll(*c);
    EXPECT_FALSE(rerr) << rerr;
    EXPECT_EQ(as_string_view(data), "bye");
    ln->Close();
}

TEST(Tls, ServerWithoutCertificate) {
    auto [ln, err] = tls::Listen("tcp", "127.0.0.1:0", tls::Config{});
    EXPECT_FALSE(ln);
    ASSERT_TRUE(err);
}
