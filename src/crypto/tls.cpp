// crypto/tls on OpenSSL with memory BIOs.
//
// The SSL object never touches the socket: TLS records produced by OpenSSL
// are taken from the write BIO and written to the net::Conn, and records
// read from the net::Conn are put into the read BIO. OpenSSL calls don't
// block, so the fiber parks only in the net::Conn's reads and writes.
//
// Locks: ssl_mu guards the SSL object and is held only around OpenSSL
// calls. read_mu and write_mu serialize readers and writers, which may run
// concurrently, as Go allows. Output is taken from the write BIO only while
// holding write_mu, so that records go out in order.
#include <gopp/crypto/tls.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace gopp {

namespace x509 {

bool CertPool::AppendCertsFromPEM(std::string_view pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    bool any = false;
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        X509_free(cert);
        any = true;
    }
    BIO_free(bio);
    ERR_clear_error();
    if (any) pems_.emplace_back(pem);
    return any;
}

}  // namespace x509

namespace tls {

namespace {

// The last OpenSSL error as text, and the error queue cleared.
std::string opensslError() {
    unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (code == 0) return "unknown error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    // "error:0A000086:SSL routines::certificate verify failed" -> the reason.
    std::string s(buf);
    auto pos = s.rfind(':');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

gopp::error tlsError(std::string_view msg) { return errors::New("tls: " + string(msg)); }

std::pair<std::string, gopp::error> read_file(std::string_view path) {
    std::ifstream f{std::string(path), std::ios::binary};
    if (!f) return {{}, errors::New("open " + string(path) + ": no such file or directory")};
    std::stringstream ss;
    ss << f.rdbuf();
    return {ss.str(), nullptr};
}

int tlsVersion(uint16 v) {
    switch (v) {
    case VersionTLS12: return TLS1_2_VERSION;
    case VersionTLS13: return TLS1_3_VERSION;
    default: return 0;
    }
}

bool loadPemChain(SSL_CTX* ctx, std::string_view cert_pem, std::string_view key_pem, std::string& err) {
    BIO* bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    X509* leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (!leaf) {
        BIO_free(bio);
        err = "failed to find any PEM data in certificate input";
        return false;
    }
    bool ok = SSL_CTX_use_certificate(ctx, leaf) == 1;
    X509_free(leaf);
    while (ok) {
        X509* extra = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!extra) break;
        ok = SSL_CTX_add0_chain_cert(ctx, extra) == 1;
    }
    BIO_free(bio);
    ERR_clear_error();  // the end of the PEM data
    if (!ok) {
        err = opensslError();
        return false;
    }
    BIO* kbio = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr);
    BIO_free(kbio);
    if (!key) {
        err = "failed to find any PEM data in key input";
        ERR_clear_error();
        return false;
    }
    ok = SSL_CTX_use_PrivateKey(ctx, key) == 1 && SSL_CTX_check_private_key(ctx) == 1;
    EVP_PKEY_free(key);
    if (!ok) err = "private key does not match public key";
    ERR_clear_error();
    return ok;
}

// ALPN: the first of the server's protocols that the client offers, as Go
// chooses.
int alpnSelect(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
                void* arg) {
    auto* protos = static_cast<std::vector<string>*>(arg);
    for (const string& p : *protos) {
        for (unsigned int i = 0; i < inlen;) {
            unsigned int n = in[i];
            if (i + 1 + n > inlen) break;
            if (n == p.size() && std::memcmp(in + i + 1, p.data(), n) == 0) {
                *out = in + i + 1;
                *outlen = static_cast<unsigned char>(n);
                return SSL_TLSEXT_ERR_OK;
            }
            i += 1 + n;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;  // no overlap: continue without ALPN
}

}  // namespace

namespace detail {

// The OpenSSL contexts compiled from a Config, one per role.
struct context {
    std::mutex mu;
    SSL_CTX* server = nullptr;
    SSL_CTX* client = nullptr;
    std::vector<string> NextProtos;
    std::string error;

    ~context() {
        if (server) SSL_CTX_free(server);
        if (client) SSL_CTX_free(client);
    }

    SSL_CTX* get(const Config& cfg, bool is_client, std::string& err) {
        std::lock_guard l(mu);
        SSL_CTX*& ctx = is_client ? client : server;
        if (ctx) return ctx;
        NextProtos = cfg.NextProtos;
        SSL_CTX* c = SSL_CTX_new(is_client ? TLS_client_method() : TLS_server_method());
        if (!c) {
            err = opensslError();
            return nullptr;
        }
        SSL_CTX_set_min_proto_version(c, tlsVersion(cfg.MinVersion));
        SSL_CTX_set_max_proto_version(c, tlsVersion(cfg.MaxVersion));
        SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_CTX_set_options(c, SSL_OP_NO_RENEGOTIATION);
        for (const Certificate& cert : cfg.Certificates) {
            if (!loadPemChain(c, cert.CertPEM, cert.KeyPEM, err)) {
                SSL_CTX_free(c);
                return nullptr;
            }
        }
        if (is_client) {
            if (!cfg.InsecureSkipVerify) {
                SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
                if (cfg.RootCAs) {
                    X509_STORE* store = SSL_CTX_get_cert_store(c);
                    for (const string& pem : cfg.RootCAs->Pems()) {
                        BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
                        while (X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
                            X509_STORE_add_cert(store, x);
                            X509_free(x);
                        }
                        BIO_free(bio);
                    }
                    ERR_clear_error();
                } else {
                    SSL_CTX_set_default_verify_paths(c);
                }
            }
            if (!NextProtos.empty()) {
                std::string wire;
                for (const string& p : NextProtos) {
                    wire.push_back(static_cast<char>(p.size()));
                    wire.append(p.view());
                }
                SSL_CTX_set_alpn_protos(c, reinterpret_cast<const unsigned char*>(wire.data()),
                                        static_cast<unsigned int>(wire.size()));
            }
        } else {
            if (cfg.Certificates.empty()) {
                SSL_CTX_free(c);
                err = "neither Certificates, GetCertificate, nor GetConfigForClient set in Config";
                return nullptr;
            }
            if (!NextProtos.empty()) SSL_CTX_set_alpn_select_cb(c, alpnSelect, &NextProtos);
        }
        ctx = c;
        return ctx;
    }
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Certificates

std::pair<Certificate, gopp::error> X509KeyPair(std::string_view cert_pem, std::string_view key_pem) {
    // Check the pair by loading it.
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    std::string err;
    bool ok = loadPemChain(ctx, cert_pem, key_pem, err);
    SSL_CTX_free(ctx);
    if (!ok) return {{}, tlsError(err)};
    return {Certificate{string(cert_pem), string(key_pem)}, nullptr};
}

std::pair<Certificate, gopp::error> LoadX509KeyPair(std::string_view cert_file, std::string_view key_file) {
    auto [cert, cerr] = read_file(cert_file);
    if (cerr) return {{}, cerr};
    auto [key, kerr] = read_file(key_file);
    if (kerr) return {{}, kerr};
    return X509KeyPair(cert, key);
}

// ---------------------------------------------------------------------------
// Conn

struct Conn::impl {
    SSL* ssl = nullptr;
    BIO* rbio = nullptr;  // network -> OpenSSL
    BIO* wbio = nullptr;  // OpenSSL -> network
    bool is_client;
    std::string setup_error;

    sync::Mutex ssl_mu;
    sync::Mutex read_mu;
    sync::Mutex write_mu;
    sync::Mutex hs_mu;
    bool handshake_done = false;
    gopp::error handshake_err;
    gopp::error readErr;
    gopp::error write_err;
    bool closed = false;
    byte inbuf[16 * 1024 + 512];

    ~impl() {
        if (ssl) SSL_free(ssl);  // frees the BIOs
    }

    // Writes the pending output of the write BIO to the connection.
    // write_mu must be held.
    gopp::error flush(net::Conn& conn) {
        std::string out;
        {
            std::lock_guard l(ssl_mu);
            char buf[16 * 1024];
            for (;;) {
                int n = BIO_read(wbio, buf, sizeof(buf));
                if (n <= 0) break;
                out.append(buf, static_cast<std::size_t>(n));
            }
        }
        if (out.empty()) return nullptr;
        if (write_err) return write_err;
        auto [n, err] = io::WriteString(conn, out);
        if (err) write_err = err;
        return err;
    }

    bool hasOutput() {
        std::lock_guard l(ssl_mu);
        return BIO_ctrl_pending(wbio) > 0;
    }

    // Reads from the connection into the read BIO. read_mu must be held.
    gopp::error fill(net::Conn& conn) {
        auto [n, err] = conn.Read(inbuf);
        if (n > 0) {
            std::lock_guard l(ssl_mu);
            BIO_write(rbio, inbuf, static_cast<int>(n));
        }
        if (n == 0 && err) return err;
        return nullptr;
    }
};

Conn::Conn(ptr<net::Conn> conn, const Config& config, bool is_client) : conn_(std::move(conn)), impl_(std::make_unique<impl>()) {
    impl_->is_client = is_client;
    if (!config.ctx_) config.ctx_ = std::make_shared<detail::context>();
    std::string err;
    SSL_CTX* ctx = config.ctx_->get(config, is_client, err);
    if (!ctx) {
        impl_->setup_error = err;
        return;
    }
    impl_->ssl = SSL_new(ctx);
    impl_->rbio = BIO_new(BIO_s_mem());
    impl_->wbio = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(impl_->rbio, -1);  // empty means "want read", not EOF
    SSL_set_bio(impl_->ssl, impl_->rbio, impl_->wbio);
    if (is_client) {
        SSL_set_connect_state(impl_->ssl);
        // The name to verify: from the config, or the host of the address.
        std::string name(config.ServerName.view());
        if (name.empty()) {
            if (auto ra = conn_->RemoteAddr()) {
                auto [host, port, herr] = net::SplitHostPort(ra->String().view());
                if (!herr) name = std::string(host.view());
            }
        }
        in6_addr a6;
        in_addr a4;
        bool is_ip = inet_pton(AF_INET, name.c_str(), &a4) == 1 || inet_pton(AF_INET6, name.c_str(), &a6) == 1;
        if (!name.empty() && !is_ip) SSL_set_tlsext_host_name(impl_->ssl, name.c_str());
        if (!config.InsecureSkipVerify && !name.empty()) {
            X509_VERIFY_PARAM* param = SSL_get0_param(impl_->ssl);
            if (is_ip) X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str());
            else X509_VERIFY_PARAM_set1_host(param, name.c_str(), 0);
        }
    } else {
        SSL_set_accept_state(impl_->ssl);
    }
}

Conn::~Conn() = default;

gopp::error Conn::Handshake() {
    std::lock_guard hl(impl_->hs_mu);
    if (impl_->handshake_done || impl_->handshake_err) return impl_->handshake_err;
    if (!impl_->ssl) {
        impl_->handshake_err = tlsError(impl_->setup_error);
        return impl_->handshake_err;
    }
    for (;;) {
        int r, e;
        long verify = X509_V_OK;
        {
            std::lock_guard l(impl_->ssl_mu);
            r = SSL_do_handshake(impl_->ssl);
            e = SSL_get_error(impl_->ssl, r);
            if (r != 1) verify = SSL_get_verify_result(impl_->ssl);
        }
        {
            std::lock_guard wl(impl_->write_mu);
            if (gopp::error err = impl_->flush(*conn_); err && r == 1) {
                impl_->handshake_err = err;
                return err;
            }
        }
        if (r == 1) {
            impl_->handshake_done = true;
            return nullptr;
        }
        if (e == SSL_ERROR_WANT_READ) {
            std::lock_guard rl(impl_->read_mu);
            if (gopp::error err = impl_->fill(*conn_)) {
                impl_->handshake_err = err;
                return err;
            }
            continue;
        }
        std::string msg = opensslError();
        if (verify != X509_V_OK) {
            msg = std::string("failed to verify certificate: x509: ") + X509_verify_cert_error_string(verify);
        }
        impl_->handshake_err = tlsError(msg);
        return impl_->handshake_err;
    }
}

std::pair<isize, gopp::error> Conn::Read(std::span<byte> p) {
    if (gopp::error err = Handshake()) return {0, err};
    std::lock_guard rl(impl_->read_mu);
    if (impl_->readErr) return {0, impl_->readErr};
    if (p.empty()) return {0, nullptr};
    for (;;) {
        std::size_t got = 0;
        int r, e;
        {
            std::lock_guard l(impl_->ssl_mu);
            r = SSL_read_ex(impl_->ssl, p.data(), p.size(), &got);
            e = r == 1 ? SSL_ERROR_NONE : SSL_get_error(impl_->ssl, r);
        }
        // Reading may produce output, such as a key update.
        if (impl_->hasOutput()) {
            std::lock_guard wl(impl_->write_mu);
            impl_->flush(*conn_);
        }
        if (r == 1) return {static_cast<isize>(got), nullptr};
        if (e == SSL_ERROR_WANT_READ) {
            if (gopp::error err = impl_->fill(*conn_)) {
                // EOF without close_notify is an unexpected EOF in Go, but
                // many servers do it; report EOF like Go's io.ReadAll sees.
                impl_->readErr = err;
                return {0, err};
            }
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN) {
            impl_->readErr = io::EOF_;  // close_notify
            return {0, io::EOF_};
        }
        impl_->readErr = tlsError(opensslError());
        return {0, impl_->readErr};
    }
}

std::pair<isize, gopp::error> Conn::Write(std::span<const byte> p) {
    if (gopp::error err = Handshake()) return {0, err};
    std::lock_guard wl(impl_->write_mu);
    if (impl_->write_err) return {0, impl_->write_err};
    isize total = 0;
    while (!p.empty()) {
        std::size_t done = 0;
        int r;
        {
            std::lock_guard l(impl_->ssl_mu);
            r = SSL_write_ex(impl_->ssl, p.data(), p.size(), &done);
        }
        if (r != 1) {
            impl_->write_err = tlsError(opensslError());
            return {total, impl_->write_err};
        }
        if (gopp::error err = impl_->flush(*conn_)) return {total, err};
        total += static_cast<isize>(done);
        p = p.subspan(done);
    }
    return {total, nullptr};
}

gopp::error Conn::Close() {
    if (impl_->ssl && impl_->handshake_done && !impl_->closed) {
        std::lock_guard wl(impl_->write_mu);
        {
            std::lock_guard l(impl_->ssl_mu);
            SSL_shutdown(impl_->ssl);
            ERR_clear_error();
        }
        // Don't wait for a slow peer when closing.
        conn_->SetWriteDeadline(time::Now().Add(5 * time::Second));
        impl_->flush(*conn_);
    }
    impl_->closed = true;
    return conn_->Close();
}

tls::ConnectionState Conn::ConnectionState() const {
    tls::ConnectionState st;
    if (!impl_->ssl) return st;
    std::lock_guard l(impl_->ssl_mu);
    st.HandshakeComplete = impl_->handshake_done;
    if (!st.HandshakeComplete) return st;
    int v = SSL_version(impl_->ssl);
    st.Version = v == TLS1_3_VERSION ? VersionTLS13 : v == TLS1_2_VERSION ? VersionTLS12 : static_cast<uint16>(v);
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(impl_->ssl, &proto, &len);
    if (proto) st.NegotiatedProtocol = string(reinterpret_cast<const char*>(proto), len);
    if (const char* sni = SSL_get_servername(impl_->ssl, TLSEXT_NAMETYPE_host_name)) st.ServerName = string(sni);
    if (const SSL_CIPHER* c = SSL_get_current_cipher(impl_->ssl)) st.CipherSuite = string(SSL_CIPHER_get_name(c));
    return st;
}

ptr<Conn> Server(ptr<net::Conn> conn, const Config& config) { return make_ptr<Conn>(std::move(conn), config, false); }
ptr<Conn> Client(ptr<net::Conn> conn, const Config& config) { return make_ptr<Conn>(std::move(conn), config, true); }

// ---------------------------------------------------------------------------
// Listener and Dial

namespace {

class listener final : public net::Listener {
public:
    listener(ptr<net::Listener> inner, Config config) : inner_(std::move(inner)), config_(std::move(config)) {}
    std::pair<ptr<net::Conn>, gopp::error> Accept() override {
        auto [c, err] = inner_->Accept();
        if (err) return {nullptr, err};
        return {Server(c, config_), nullptr};
    }
    gopp::error Close() override { return inner_->Close(); }
    ptr<net::Addr> Addr() const override { return inner_->Addr(); }

private:
    ptr<net::Listener> inner_;
    Config config_;
};

}  // namespace

ptr<net::Listener> NewListener(ptr<net::Listener> inner, const Config& config) {
    return make_ptr<listener>(std::move(inner), config);
}

std::pair<ptr<net::Listener>, gopp::error> Listen(std::string_view network, std::string_view address, const Config& config) {
    if (config.Certificates.empty()) {
        return {nullptr, tlsError("neither Certificates, GetCertificate, nor GetConfigForClient set in Config")};
    }
    auto [ln, err] = net::Listen(network, address);
    if (err) return {nullptr, err};
    return {NewListener(ln, config), nullptr};
}

std::pair<ptr<Conn>, gopp::error> Dial(std::string_view network, std::string_view address, const Config& config) {
    auto [raw, err] = net::Dial(network, address);
    if (err) return {nullptr, err};
    Config cfg = config;
    if (cfg.ServerName.empty()) {
        auto [host, port, herr] = net::SplitHostPort(address);
        if (!herr) cfg.ServerName = host;
        cfg.ctx_ = config.ctx_;
    }
    auto c = Client(raw, cfg);
    if (gopp::error herr = c->Handshake()) {
        raw->Close();
        return {nullptr, herr};
    }
    return {c, nullptr};
}

}  // namespace tls

}  // namespace gopp
