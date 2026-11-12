// The crypto/tls package, shaped like Go's, on OpenSSL.
//
// TLS runs over any net::Conn: OpenSSL works on memory buffers and go++
// does the network I/O, so handshakes, reads and writes park the fiber like
// any other network operation. As with Go's tls.Conn, one fiber may read
// while another writes.
//
//     tls::Config cfg;
//     auto [cert, err] = tls::LoadX509KeyPair("cert.pem", "key.pem");
//     cfg.Certificates = {cert};
//     auto [ln, lerr] = tls::Listen("tcp", ":8443", cfg);
//
//     auto [conn, derr] = tls::Dial("tcp", "example.com:443", tls::Config{});
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/net.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <memory>
#include <string_view>
#include <vector>
#endif

namespace gopp::x509 {

// x509.CertPool: trusted root certificates, as PEM.
class CertPool final : public object {
public:
    // Adds the certificates in pem; false if it holds none.
    bool AppendCertsFromPEM(std::string_view pem);
    const std::vector<string>& Pems() const noexcept { return pems_; }

private:
    std::vector<string> pems_;
};

// x509.NewCertPool
inline ptr<CertPool> NewCertPool() { return make_ptr<CertPool>(); }

}  // namespace gopp::x509

namespace gopp::tls {

inline constexpr uint16 VersionTLS12 = 0x0303;
inline constexpr uint16 VersionTLS13 = 0x0304;

// tls.Certificate: a certificate chain and its private key, as PEM.
struct Certificate {
    string CertPEM;  // the leaf first, then intermediates
    string KeyPEM;
};

// tls.LoadX509KeyPair: reads and checks a PEM certificate and key.
std::pair<Certificate, error> LoadX509KeyPair(std::string_view cert_file, std::string_view key_file);
// tls.X509KeyPair
std::pair<Certificate, error> X509KeyPair(std::string_view cert_pem, std::string_view key_pem);

namespace detail {
struct context;  // compiled OpenSSL contexts of a Config
}

// tls.Config. Don't modify a Config once it is in use.
struct Config {
    // Certificates to present: the server's, or a client certificate.
    std::vector<Certificate> Certificates{};
    // ALPN protocols, in order of preference ("h2", "http/1.1").
    std::vector<string> NextProtos{};
    // The host name to verify and send as SNI; from the address if empty.
    string ServerName{};
    // Skip verifying the server's certificate chain and host name.
    bool InsecureSkipVerify = false;
    // Trusted roots; the system's if nil.
    ptr<x509::CertPool> RootCAs{};
    uint16 MinVersion = VersionTLS12;
    uint16 MaxVersion = 0;  // the highest supported if 0

    // Compiled on first use and shared by copies.
    mutable std::shared_ptr<detail::context> ctx_{};
};

// tls.ConnectionState
struct ConnectionState {
    uint16 Version = 0;
    bool HandshakeComplete = false;
    string NegotiatedProtocol;  // from ALPN, or empty
    string ServerName;          // SNI: sent by the client, received by the server
    string CipherSuite;
};

// tls.Conn
class Conn final : public net::Conn {
public:
    Conn(ptr<net::Conn> conn, const Config& config, bool is_client);
    ~Conn() override;

    // Runs the handshake if it hasn't run; read and write run it too.
    error Handshake();

    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Sends close_notify and closes the connection.
    error Close() override;
    string LocalAddr() const override { return conn_->LocalAddr(); }
    string RemoteAddr() const override { return conn_->RemoteAddr(); }
    error SetDeadline(time::Time t) override { return conn_->SetDeadline(t); }
    error SetReadDeadline(time::Time t) override { return conn_->SetReadDeadline(t); }
    error SetWriteDeadline(time::Time t) override { return conn_->SetWriteDeadline(t); }

    tls::ConnectionState ConnectionState() const;
    // The underlying connection.
    const ptr<net::Conn>& NetConn() const noexcept { return conn_; }

private:
    struct impl;
    ptr<net::Conn> conn_;
    std::unique_ptr<impl> impl_;
};

// tls.Server and tls.Client: TLS over an established connection. The
// handshake runs on the first read or write, or with handshake().
ptr<Conn> Server(ptr<net::Conn> conn, const Config& config);
ptr<Conn> Client(ptr<net::Conn> conn, const Config& config);

// tls.NewListener: accepts TLS server connections from inner.
ptr<net::Listener> NewListener(ptr<net::Listener> inner, const Config& config);
// tls.Listen
std::pair<ptr<net::Listener>, error> Listen(std::string_view network, std::string_view address, const Config& config);
// tls.Dial: connects and completes the handshake.
std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address, const Config& config);

}  // namespace gopp::tls
