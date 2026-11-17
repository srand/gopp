// crypto/ecdsa on OpenSSL 3's EVP_PKEY: keys and ASN.1 signatures.
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/sync.hpp>

#include "crypto/pkey.hpp"

#include <openssl/ec.h>

#include <vector>

namespace gopp::ecdsa {

namespace {

constexpr const char* pkg = "crypto/ecdsa";

// OpenSSL's name for c, or nullptr if c isn't one of the four singletons.
const char* groupName(const ptr<elliptic::Curve>& c) {
    if (!c) return nullptr;
    if (c.get() == elliptic::P224().get()) return "P-224";
    if (c.get() == elliptic::P256().get()) return "P-256";
    if (c.get() == elliptic::P384().get()) return "P-384";
    if (c.get() == elliptic::P521().get()) return "P-521";
    return nullptr;
}

error unsupportedCurve() { return errors::New("crypto/ecdsa: unsupported curve"); }

// The uncompressed SEC 1 encoding of (x, y), which OpenSSL checks to be a
// point of the curve. Like Go's pointFromAffine, rejects coordinates that
// would not encode.
std::pair<std::vector<byte>, error> encodePoint(const elliptic::Curve& c, const big::Int& x, const big::Int& y) {
    int bitSize = c.Params().BitSize;
    if (x.Sign() < 0 || y.Sign() < 0) return {{}, errors::New("negative coordinate")};
    if (x.BitLen() > bitSize || y.BitLen() > bitSize) return {{}, errors::New("overflowing coordinate")};
    size_t byteLen = static_cast<size_t>((bitSize + 7) / 8);
    std::vector<byte> buf(1 + 2 * byteLen);
    buf[0] = byte(4);
    x.FillBytes(std::span<byte>(buf.data() + 1, byteLen));
    y.FillBytes(std::span<byte>(buf.data() + 1 + byteLen, byteLen));
    return {std::move(buf), nullptr};
}

// The point of pub added to b, or an error.
error addPublic(pkey::ParamBuilder& b, const PublicKey& pub, std::vector<byte>& point) {
    const char* name = groupName(pub.Curve);
    if (!name) return unsupportedCurve();
    auto [p, err] = encodePoint(*pub.Curve, pub.X, pub.Y);
    if (err) return errors::New(string("crypto/ecdsa: invalid public key: " + std::string(err->Error().view())));
    point = std::move(p);
    b.addUtf8(OSSL_PKEY_PARAM_GROUP_NAME, name);
    b.addOctets(OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
    return nullptr;
}

// The key as an EVP_PKEY, built on first use. OpenSSL rejects a point that is
// not on the curve, at infinity, or not reduced.
std::pair<EVP_PKEY*, error> publicEvp(const PublicKey& pub) {
    return pkey::cached(pub.cache_, [&]() -> std::pair<pkey::Key, error> {
        pkey::ParamBuilder b;
        std::vector<byte> point;
        if (error err = addPublic(b, pub, point)) {
            return {pkey::Key(), err};
        }
        auto [k, err] = pkey::fromParams("EC", EVP_PKEY_PUBLIC_KEY, b, pkg);
        return {std::move(k), err};
    });
}

std::pair<EVP_PKEY*, error> privateEvp(const PrivateKey& priv) {
    return pkey::cached(priv.cache_, [&]() -> std::pair<pkey::Key, error> {
        const PublicKey& pub = priv.PublicKey;
        pkey::ParamBuilder b;
        std::vector<byte> point;
        if (error err = addPublic(b, pub, point)) {
            return {pkey::Key(), err};
        }
        if (priv.D.Sign() <= 0 || priv.D.Cmp(pub.Curve->Params().N) >= 0) {
            return {pkey::Key(), errors::New("crypto/ecdsa: invalid private key")};
        }
        b.add(OSSL_PKEY_PARAM_PRIV_KEY, priv.D);
        auto [k, err] = pkey::fromParams("EC", EVP_PKEY_KEYPAIR, b, pkg);
        if (err) {
            return {pkey::Key(), err};
        }
        // The public key must be the private key's.
        pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, k.get(), nullptr));
        if (!ctx || EVP_PKEY_check(ctx.get()) != 1) {
            ERR_clear_error();
            return {pkey::Key(), errors::New("crypto/ecdsa: invalid private key")};
        }
        return {std::move(k), nullptr};
    });
}

// One DER element at the front of in: its tag, and its content, and what
// follows. As cryptobyte reads it: the shortest form of the length, and no
// indefinite lengths.
bool readElement(std::span<const byte> in, unsigned tag, std::span<const byte>& content, std::span<const byte>& rest) {
    if (in.size() < 2 || static_cast<unsigned>(in[0]) != tag) return false;
    size_t len = static_cast<unsigned>(in[1]), hdr = 2;
    if (len & 0x80) {
        size_t n = len & 0x7f;
        if (n == 0 || n > 4 || in.size() < 2 + n) return false;
        len = 0;
        for (size_t i = 0; i < n; i++) len = len << 8 | static_cast<unsigned>(in[2 + i]);
        // Not the shortest: a leading zero, or one that fits the short form.
        if (static_cast<unsigned>(in[2]) == 0 || len < 0x80) return false;
        hdr = 2 + n;
    }
    if (in.size() - hdr < len) return false;
    content = in.subspan(hdr, len);
    rest = in.subspan(hdr + len);
    return true;
}

// A non-negative INTEGER of minimal encoding: its value as bytes, without
// the leading zero.
bool readUnsigned(std::span<const byte>& in, std::span<const byte>& value) {
    std::span<const byte> content, rest;
    if (!readElement(in, 0x02, content, rest) || content.empty()) return false;
    in = rest;
    if (static_cast<unsigned>(content[0]) & 0x80) return false;  // negative
    if (content.size() > 1 && content[0] == byte(0) && !(static_cast<unsigned>(content[1]) & 0x80)) return false;
    value = content;
    return true;
}

// Go's parseSignature.
bool parseSignature(std::span<const byte> sig, std::span<const byte>& r, std::span<const byte>& s) {
    std::span<const byte> inner, rest;
    if (!readElement(sig, 0x30, inner, rest) || !rest.empty()) return false;
    return readUnsigned(inner, r) && readUnsigned(inner, s) && inner.empty();
}

// Whether the value is in 1 to N-1.
bool inRange(std::span<const byte> v, const big::Int& N) {
    big::Int x;
    x.SetBytes(v);
    return x.Sign() > 0 && x.Cmp(N) < 0;
}

}  // namespace

bool PublicKey::Equal(const crypto::PublicKey& x) const {
    auto xx = dynamic_cast<const ecdsa::PublicKey*>(&x);
    // The standard curves are singletons, so comparing pointers is Go's check.
    return xx && X.Cmp(xx->X) == 0 && Y.Cmp(xx->Y) == 0 && Curve.get() == xx->Curve.get();
}

ptr<crypto::PublicKey> PrivateKey::Public() const { return make_ptr<ecdsa::PublicKey>(PublicKey); }

std::pair<slice<byte>, error> PrivateKey::Sign(io::Reader* rand, std::span<const byte> digest,
                                               const crypto::SignerOpts&) {
    return SignASN1(rand, *this, digest);
}

std::pair<ptr<PrivateKey>, error> GenerateKey(ptr<elliptic::Curve> c, io::Reader*) {
    const char* name = groupName(c);
    if (!name) return {nullptr, unsupportedCurve()};
    pkey::Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", name));
    if (!key) return {nullptr, pkey::openSSLError(pkg, "generating the key")};
    size_t byteLen = static_cast<size_t>((c->Params().BitSize + 7) / 8);
    std::vector<unsigned char> point(1 + 2 * byteLen);
    size_t n = 0;
    auto [d, okD] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_PRIV_KEY);
    if (!okD || EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(), &n) != 1 ||
        n != point.size() || point[0] != 4)
        return {nullptr, pkey::openSSLError(pkg, "reading the key")};
    auto priv = make_ptr<PrivateKey>();
    priv->PublicKey.Curve = c;
    priv->PublicKey.X.SetBytes(std::span<const byte>(reinterpret_cast<const byte*>(point.data()) + 1, byteLen));
    priv->PublicKey.Y.SetBytes(std::span<const byte>(reinterpret_cast<const byte*>(point.data()) + 1 + byteLen, byteLen));
    priv->D = std::move(d);
    return {priv, nullptr};
}

std::pair<slice<byte>, error> SignASN1(io::Reader*, const PrivateKey& priv, std::span<const byte> hash) {
    auto [key, kerr] = privateEvp(priv);
    if (kerr) return {slice<byte>(), kerr};
    pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx || EVP_PKEY_sign_init(ctx.get()) != 1) return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    size_t n = 0;
    if (EVP_PKEY_sign(ctx.get(), nullptr, &n, reinterpret_cast<const unsigned char*>(hash.data()), hash.size()) != 1)
        return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    std::vector<unsigned char> sig(n);
    if (EVP_PKEY_sign(ctx.get(), sig.data(), &n, reinterpret_cast<const unsigned char*>(hash.data()), hash.size()) != 1)
        return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    return {appendSpan(slice<byte>(), std::span<const byte>(reinterpret_cast<const byte*>(sig.data()), n)), nullptr};
}

std::tuple<big::Int, big::Int, error> Sign(io::Reader* rand, const PrivateKey& priv, std::span<const byte> hash) {
    auto [sig, err] = SignASN1(rand, priv, hash);
    if (err) return {big::Int(), big::Int(), err};
    std::span<const byte> rb, sb;
    if (!parseSignature(sig, rb, sb)) return {big::Int(), big::Int(), errors::New("invalid ASN.1 from SignASN1")};
    big::Int r, s;
    r.SetBytes(rb);
    s.SetBytes(sb);
    return {std::move(r), std::move(s), nullptr};
}

bool VerifyASN1(const PublicKey& pub, std::span<const byte> hash, std::span<const byte> sig) {
    if (!groupName(pub.Curve)) return false;
    std::span<const byte> rb, sb;
    if (!parseSignature(sig, rb, sb)) return false;
    const big::Int& N = pub.Curve->Params().N;
    if (!inRange(rb, N) || !inRange(sb, N)) return false;
    auto [key, kerr] = publicEvp(pub);
    if (kerr) return false;
    pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx || EVP_PKEY_verify_init(ctx.get()) != 1) {
        ERR_clear_error();
        return false;
    }
    int ok = EVP_PKEY_verify(ctx.get(), reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                             reinterpret_cast<const unsigned char*>(hash.data()), hash.size());
    ERR_clear_error();
    return ok == 1;
}

bool Verify(const PublicKey& pub, std::span<const byte> hash, const big::Int& r, const big::Int& s) {
    if (r.Sign() <= 0 || s.Sign() <= 0) return false;
    // The DER of (r, s): Go's encodeSignature.
    ECDSA_SIG* sig = ECDSA_SIG_new();
    if (!sig) return false;
    BIGNUM* rb = BN_dup(pkey::bn(r));
    BIGNUM* sb = BN_dup(pkey::bn(s));
    if (!rb || !sb || ECDSA_SIG_set0(sig, rb, sb) != 1) {
        BN_free(rb);
        BN_free(sb);
        ECDSA_SIG_free(sig);
        return false;
    }
    unsigned char* der = nullptr;
    int n = i2d_ECDSA_SIG(sig, &der);
    ECDSA_SIG_free(sig);
    if (n <= 0) return false;
    bool ok = VerifyASN1(pub, hash, std::span<const byte>(reinterpret_cast<const byte*>(der), static_cast<size_t>(n)));
    OPENSSL_free(der);
    return ok;
}

}  // namespace gopp::ecdsa
