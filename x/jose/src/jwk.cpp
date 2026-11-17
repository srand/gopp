// JSON Web Keys, ported from go-jose v4.1.7's jwk.go and encoding.go.
#include "internal.hpp"

#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/hex.hpp>

#include <openssl/bn.h>

#include <array>
#include <climits>
#include <memory>
#include <vector>

namespace gopp::jose {

const error ErrUnsupportedKeyType = errors::New("go-jose/go-jose: unsupported key type/format");
const error ErrUnsupportedEllipticCurve = errors::New("go-jose/go-jose: unsupported/unknown elliptic curve");
int MaxRSAModulusBits = 4096;

namespace internal {

namespace {

error jerr(std::string_view msg) { return errors::New(string("go-jose/go-jose: ") + string(msg)); }

// "X, Y" for the names of the missing members.
string joinMissing(const std::vector<std::string_view>& missing) {
    std::string s;
    for (auto m : missing) {
        if (!s.empty()) s += ", ";
        s += m;
    }
    return string(s);
}

const BIGNUM* bn(const big::Int& x) { return static_cast<const BIGNUM*>(x.bn()); }

struct CtxDeleter {
    void operator()(BN_CTX* c) const { BN_CTX_free(c); }
};
struct BnDeleter {
    void operator()(BIGNUM* b) const { BN_free(b); }
};
using Ctx = std::unique_ptr<BN_CTX, CtxDeleter>;
using Bn = std::unique_ptr<BIGNUM, BnDeleter>;

// Whether p * q is n.
bool productIs(const big::Int& p, const big::Int& q, const big::Int& n) {
    Ctx ctx(BN_CTX_new());
    Bn z(BN_new());
    return ctx && z && BN_mul(z.get(), bn(p), bn(q), ctx.get()) == 1 && BN_cmp(z.get(), bn(n)) == 0;
}

// elliptic.Curve.IsOnCurve: whether x and y are in the field and satisfy
// y^2 = x^3 - 3x + b.
bool isOnCurve(const elliptic::CurveParams& c, const big::Int& x, const big::Int& y) {
    if (x.Sign() < 0 || y.Sign() < 0 || x.Cmp(c.P) >= 0 || y.Cmp(c.P) >= 0) return false;
    Ctx ctx(BN_CTX_new());
    Bn y2(BN_new()), x3(BN_new()), x3a(BN_new()), rhs(BN_new());
    if (!ctx || !y2 || !x3 || !x3a || !rhs) return false;
    const BIGNUM* p = bn(c.P);
    bool ok = BN_mod_sqr(y2.get(), bn(y), p, ctx.get()) == 1 &&         // y^2
              BN_mod_sqr(x3.get(), bn(x), p, ctx.get()) == 1 &&         // x^2
              BN_mod_mul(x3.get(), x3.get(), bn(x), p, ctx.get()) == 1 &&  // x^3
              BN_mod_add(x3a.get(), bn(x), bn(x), p, ctx.get()) == 1 &&  // 2x
              BN_mod_add(x3a.get(), x3a.get(), bn(x), p, ctx.get()) == 1 &&  // 3x
              BN_mod_sub(rhs.get(), x3.get(), x3a.get(), p, ctx.get()) == 1 &&
              BN_mod_add(rhs.get(), rhs.get(), bn(c.B), p, ctx.get()) == 1;
    return ok && BN_cmp(y2.get(), rhs.get()) == 0;
}

// The low-order Ed25519 encodings that Go's verifier accepts and that must
// not be accepted as JOSE verification keys.
const std::vector<slice<byte>>& weakEd25519PublicKeys() {
    static const std::vector<slice<byte>> keys = [] {
        std::vector<slice<byte>> v;
        for (std::string_view h : {
                 "0000000000000000000000000000000000000000000000000000000000000000",
                 "0000000000000000000000000000000000000000000000000000000000000080",
                 "0100000000000000000000000000000000000000000000000000000000000000",
                 "0100000000000000000000000000000000000000000000000000000000000080",
                 "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
                 "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
                 "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
                 "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
                 "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                 "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                 "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                 "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                 "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                 "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             }) {
            v.push_back(hex::DecodeString(h).first);
        }
        return v;
    }();
    return keys;
}

bool equalBytes(std::span<const byte> a, std::span<const byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

std::string stripWhitespace(std::string_view data) {
    std::string out;
    out.reserve(data.size());
    for (char c : data) {
        if (c == ' ' || (c >= '\t' && c <= '\r')) continue;
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// byteBuffer

std::pair<slice<byte>, error> byteBuffer::MarshalJSON() const { return json::Marshal(base64()); }

error byteBuffer::UnmarshalJSON(std::span<const byte> d) {
    string encoded;
    if (error err = json::Unmarshal(d, &encoded)) return err;
    if (encoded.empty()) return nullptr;
    auto [decoded, err] = base64::RawURLEncoding.DecodeString(encoded);
    if (err) return err;
    data = decoded;
    return nullptr;
}

string byteBuffer::base64() const { return base64::RawURLEncoding.EncodeToString(data); }

big::Int byteBuffer::bigInt() const {
    big::Int n;
    n.SetBytes(data);
    return n;
}

byteBuffer newBuffer(std::span<const byte> data) { return byteBuffer{slice<byte>::of(data)}; }

byteBuffer newFixedSizeBuffer(std::span<const byte> data, int length) {
    if (data.size() > static_cast<size_t>(length)) {
        panic("go-jose/go-jose: invalid call to newFixedSizeBuffer (len(data) > length)");
    }
    auto buf = slice<byte>::make(length);
    std::copy(data.begin(), data.end(), buf.begin() + (length - static_cast<int>(data.size())));
    return byteBuffer{buf};
}

byteBuffer newBufferFromInt(uint64_t num) {
    std::array<byte, 8> b;
    for (int i = 0; i < 8; i++) b[i] = static_cast<byte>(num >> (8 * (7 - i)));
    size_t skip = 0;
    while (skip < b.size() && b[skip] == 0) skip++;
    return newBuffer(std::span<const byte>(b).subspan(skip));
}

// ---------------------------------------------------------------------------
// Curves

std::pair<string, error> curveName(const ptr<elliptic::Curve>& crv) {
    if (crv && crv.get() == elliptic::P256().get()) return {"P-256", nullptr};
    if (crv && crv.get() == elliptic::P384().get()) return {"P-384", nullptr};
    if (crv && crv.get() == elliptic::P521().get()) return {"P-521", nullptr};
    return {string(), ErrUnsupportedEllipticCurve};
}

int curveSize(const elliptic::Curve& crv) {
    int bits = crv.Params().BitSize;
    return bits / 8 + (bits % 8 != 0 ? 1 : 0);
}

// The length of this octet string MUST be ceiling(log-base-2(n)/8)
// octets (where n is the order of the curve).
// https://tools.ietf.org/html/rfc7518#section-6.2.2.1
int dSize(const elliptic::Curve& crv) {
    int bitLen = crv.Params().P.BitLen();
    return bitLen / 8 + (bitLen % 8 != 0 ? 1 : 0);
}

namespace {

std::pair<ptr<elliptic::Curve>, error> curveOf(const string& crv) {
    if (crv == "P-256") return {elliptic::P256(), nullptr};
    if (crv == "P-384") return {elliptic::P384(), nullptr};
    if (crv == "P-521") return {elliptic::P521(), nullptr};
    return {nullptr, errors::New("go-jose/go-jose: unsupported elliptic curve '" + crv + "'")};
}

}  // namespace

// ---------------------------------------------------------------------------
// Ed25519

error validateEd25519PublicKey(std::span<const byte> publicKey) {
    if (publicKey.size() != static_cast<size_t>(ed25519::PublicKeySize)) {
        return jerr("invalid Ed25519 public key, wrong length for x");
    }
    for (auto& weak : weakEd25519PublicKeys()) {
        if (equalBytes(weak, publicKey)) return jerr("invalid Ed25519 public key, low-order point");
    }
    return nullptr;
}

std::pair<ptr<ed25519::PublicKey>, error> rawJSONWebKey::edPublicKey() const {
    if (!X) return {nullptr, jerr("invalid Ed key, missing x value")};
    auto publicKey = X->bytes();
    if (error err = validateEd25519PublicKey(publicKey)) return {nullptr, err};
    auto pub = make_ptr<ed25519::PublicKey>();
    pub->key = slice<byte>::of(publicKey);
    return {pub, nullptr};
}

std::pair<ptr<ed25519::PrivateKey>, error> rawJSONWebKey::edPrivateKey() const {
    std::vector<std::string_view> missing;
    if (!D) missing.push_back("D");
    if (!X) missing.push_back("X");
    if (!missing.empty()) {
        return {nullptr, errors::New("go-jose/go-jose: invalid Ed25519 private key, missing " + joinMissing(missing) +
                                     " value(s)")};
    }
    auto publicKey = X->bytes();
    if (error err = validateEd25519PublicKey(publicKey)) return {nullptr, err};
    auto seed = D->bytes();
    if (seed.size() != static_cast<size_t>(ed25519::SeedSize)) {
        return {nullptr, jerr("invalid Ed25519 private key, wrong length for d")};
    }
    auto privateKey = ed25519::NewKeyFromSeed(seed);
    if (!equalBytes(privateKey->key.span().subspan(32), publicKey)) {
        return {nullptr, jerr("invalid Ed25519 private key, x does not match d")};
    }
    return {privateKey, nullptr};
}

std::pair<rawJSONWebKey, error> fromEdPublicKey(const ed25519::PublicKey& pub) {
    if (error err = validateEd25519PublicKey(pub.key)) return {{}, err};
    rawJSONWebKey raw;
    raw.Kty = "OKP";
    raw.Crv = "Ed25519";
    raw.X = newBuffer(pub.key);
    return {raw, nullptr};
}

std::pair<rawJSONWebKey, error> fromEdPrivateKey(const ed25519::PrivateKey& ed) {
    if (ed.key.size() != static_cast<size_t>(ed25519::PrivateKeySize)) {
        return {{}, jerr("invalid Ed25519 private key length")};
    }
    ed25519::PublicKey pub;
    pub.key = ed.key.Sub(32);
    auto [raw, err] = fromEdPublicKey(pub);
    if (err) return {{}, err};
    raw.D = newBuffer(ed.key.Sub(0, 32));
    return {raw, nullptr};
}

// ---------------------------------------------------------------------------
// RSA

namespace {

std::pair<ptr<rsa::PublicKey>, error> rsaPublicKeyOf(const std::optional<byteBuffer>& N, const std::optional<byteBuffer>& E) {
    if (!N || !E) return {nullptr, jerr("invalid RSA key, missing n/e values")};

    big::Int n = N->bigInt();
    if (n.BitLen() > MaxRSAModulusBits) return {nullptr, jerr("invalid RSA key, modulus too large")};

    big::Int e = E->bigInt();
    if (!e.IsInt64()) return {nullptr, jerr("invalid RSA key, bad e value")};
    // The exponent of rsa::PublicKey is an int.
    int64 e64 = e.Int64();
    if (e64 < 2 || e64 > INT_MAX) return {nullptr, jerr("invalid RSA key, bad e value")};

    auto pub = make_ptr<rsa::PublicKey>();
    pub->N = std::move(n);
    pub->E = static_cast<int>(e64);
    return {pub, nullptr};
}

}  // namespace

std::pair<ptr<rsa::PublicKey>, error> rawJSONWebKey::rsaPublicKey() const { return rsaPublicKeyOf(N, E); }

std::pair<ptr<rsa::PrivateKey>, error> rawJSONWebKey::rsaPrivateKey() const {
    std::vector<std::string_view> missing;
    if (!N) missing.push_back("N");
    if (!E) missing.push_back("E");
    if (!D) missing.push_back("D");
    if (!P) missing.push_back("P");
    if (!Q) missing.push_back("Q");
    if (!missing.empty()) {
        return {nullptr,
                errors::New("go-jose/go-jose: invalid RSA private key, missing " + joinMissing(missing) + " value(s)")};
    }

    auto [pubKey, err] = rsaPublicKeyOf(N, E);
    if (err) return {nullptr, err};

    const big::Int& n = pubKey->N;
    big::Int p = P->bigInt(), q = Q->bigInt();
    if (p.BitLen() + q.BitLen() > n.BitLen() + 1 || !productIs(p, q, n)) {
        return {nullptr, jerr("invalid RSA private key, p * q != n")};
    }

    auto rv = make_ptr<rsa::PrivateKey>();
    rv->PublicKey.N = n;
    rv->PublicKey.E = pubKey->E;
    rv->D = D->bigInt();
    rv->Primes.push_back(std::move(p));
    rv->Primes.push_back(std::move(q));
    if (error verr = rv->Validate()) return {nullptr, verr};
    return {rv, nullptr};
}

rawJSONWebKey fromRsaPublicKey(const rsa::PublicKey& pub) {
    rawJSONWebKey raw;
    raw.Kty = "RSA";
    raw.N = newBuffer(pub.N.Bytes());
    raw.E = newBufferFromInt(static_cast<uint64_t>(static_cast<int64>(pub.E)));
    return raw;
}

std::pair<rawJSONWebKey, error> fromRsaPrivateKey(const rsa::PrivateKey& rsa) {
    if (rsa.Primes.size() != 2) return {{}, ErrUnsupportedKeyType};
    rawJSONWebKey raw = fromRsaPublicKey(rsa.PublicKey);
    raw.D = newBuffer(rsa.D.Bytes());
    raw.P = newBuffer(rsa.Primes[0].Bytes());
    raw.Q = newBuffer(rsa.Primes[1].Bytes());
    return {raw, nullptr};
}

// ---------------------------------------------------------------------------
// EC

std::pair<ptr<ecdsa::PublicKey>, error> rawJSONWebKey::ecPublicKey() const {
    auto [curve, cerr] = curveOf(Crv);
    if (cerr) return {nullptr, cerr};
    if (!X || !Y) return {nullptr, jerr("invalid EC key, missing x/y values")};

    // The length of this octet string MUST be the full size of a coordinate for
    // the curve specified in the "crv" parameter.
    // https://tools.ietf.org/html/rfc7518#section-6.2.1.2
    size_t size = static_cast<size_t>(curveSize(*curve));
    if (size != X->bytes().size()) return {nullptr, jerr("invalid EC public key, wrong length for x")};
    if (size != Y->bytes().size()) return {nullptr, jerr("invalid EC public key, wrong length for y")};

    big::Int x = X->bigInt(), y = Y->bigInt();
    if (!isOnCurve(curve->Params(), x, y)) return {nullptr, jerr("invalid EC key, X/Y are not on declared curve")};

    auto pub = make_ptr<ecdsa::PublicKey>();
    pub->Curve = curve;
    pub->X = std::move(x);
    pub->Y = std::move(y);
    return {pub, nullptr};
}

std::pair<ptr<ecdsa::PrivateKey>, error> rawJSONWebKey::ecPrivateKey() const {
    auto [curve, cerr] = curveOf(Crv);
    if (cerr) return {nullptr, cerr};

    std::vector<std::string_view> missing;
    if (!X) missing.push_back("X");
    if (!Y) missing.push_back("Y");
    if (!D) missing.push_back("D");
    if (!missing.empty()) {
        return {nullptr,
                errors::New("go-jose/go-jose: invalid EC private key, missing " + joinMissing(missing) + " value(s)")};
    }

    size_t size = static_cast<size_t>(curveSize(*curve));
    if (size != X->bytes().size()) return {nullptr, jerr("invalid EC private key, wrong length for x")};
    if (size != Y->bytes().size()) return {nullptr, jerr("invalid EC private key, wrong length for y")};
    // https://tools.ietf.org/html/rfc7518#section-6.2.2.1
    if (static_cast<size_t>(dSize(*curve)) != D->bytes().size()) {
        return {nullptr, jerr("invalid EC private key, wrong length for d")};
    }

    big::Int x = X->bigInt(), y = Y->bigInt();
    if (!isOnCurve(curve->Params(), x, y)) return {nullptr, jerr("invalid EC key, X/Y are not on declared curve")};

    auto priv = make_ptr<ecdsa::PrivateKey>();
    priv->PublicKey.Curve = curve;
    priv->PublicKey.X = std::move(x);
    priv->PublicKey.Y = std::move(y);
    priv->D = D->bigInt();
    return {priv, nullptr};
}

std::pair<rawJSONWebKey, error> fromEcPublicKey(const ecdsa::PublicKey& pub) {
    // Go's nil X and Y are zero here.
    if (pub.X.Sign() == 0 || pub.Y.Sign() == 0) return {{}, jerr("invalid EC key (nil, or X/Y missing)")};
    auto [name, err] = curveName(pub.Curve);
    if (err) return {{}, err};
    int size = curveSize(*pub.Curve);
    auto xBytes = pub.X.Bytes(), yBytes = pub.Y.Bytes();
    if (xBytes.size() > static_cast<size_t>(size) || yBytes.size() > static_cast<size_t>(size)) {
        return {{}, jerr("invalid EC key (X/Y too large)")};
    }
    rawJSONWebKey raw;
    raw.Kty = "EC";
    raw.Crv = name;
    raw.X = newFixedSizeBuffer(xBytes, size);
    raw.Y = newFixedSizeBuffer(yBytes, size);
    return {raw, nullptr};
}

std::pair<rawJSONWebKey, error> fromEcPrivateKey(const ecdsa::PrivateKey& ec) {
    auto [raw, err] = fromEcPublicKey(ec.PublicKey);
    if (err) return {{}, err};
    if (ec.D.Sign() == 0) return {{}, jerr("invalid EC private key")};
    auto d = ec.D.Bytes();
    int size = dSize(*ec.PublicKey.Curve);
    if (d.size() > static_cast<size_t>(size)) return {{}, jerr("invalid EC private key (D too large)")};
    raw.D = newFixedSizeBuffer(d, size);
    return {raw, nullptr};
}

}  // namespace internal

// ---------------------------------------------------------------------------
// JSONWebKey

namespace {

using internal::rawJSONWebKey;

// The type of a key that go-jose doesn't know, for an error.
string typeOf(const ptr<object>& key) {
    if (!key) return "<nil>";
    return string(typeid(*key).name());
}

}  // namespace

std::pair<slice<byte>, error> JSONWebKey::MarshalJSON() const {
    rawJSONWebKey raw;
    error err;
    object* key = Key.get();
    if (auto* k = dynamic_cast<ed25519::PublicKey*>(key)) {
        std::tie(raw, err) = internal::fromEdPublicKey(*k);
    } else if (auto* k = dynamic_cast<ecdsa::PublicKey*>(key)) {
        std::tie(raw, err) = internal::fromEcPublicKey(*k);
    } else if (auto* k = dynamic_cast<rsa::PublicKey*>(key)) {
        raw = internal::fromRsaPublicKey(*k);
    } else if (auto* k = dynamic_cast<ed25519::PrivateKey*>(key)) {
        std::tie(raw, err) = internal::fromEdPrivateKey(*k);
    } else if (auto* k = dynamic_cast<ecdsa::PrivateKey*>(key)) {
        std::tie(raw, err) = internal::fromEcPrivateKey(*k);
    } else if (auto* k = dynamic_cast<rsa::PrivateKey*>(key)) {
        std::tie(raw, err) = internal::fromRsaPrivateKey(*k);
    } else {
        return {slice<byte>(), errors::New("go-jose/go-jose: unknown key type '" + typeOf(Key) + "'")};
    }
    if (err) return {slice<byte>(), err};

    raw.Kid = KeyID;
    raw.Alg = Algorithm;
    raw.Use = Use;
    return json::Marshal(raw);
}

error JSONWebKey::UnmarshalJSON(std::span<const byte> data) {
    rawJSONWebKey raw;
    if (error err = json::Unmarshal(data, &raw)) return err;

    // RFC 7517 section 4.7 asks for the key of an x5c to be the key of the
    // JWK; go++ doesn't parse certificates, and ignores x5c, x5t, x5t#S256
    // and x5u, which the key of a JWKS is verified without.
    ptr<object> key;
    error err;
    auto either = [&](auto&& f) {
        auto [k, e] = f();
        key = k;
        err = e;
    };
    if (raw.Kty == "EC") {
        if (raw.D) either([&] { return raw.ecPrivateKey(); });
        else either([&] { return raw.ecPublicKey(); });
    } else if (raw.Kty == "RSA") {
        if (raw.D) either([&] { return raw.rsaPrivateKey(); });
        else either([&] { return raw.rsaPublicKey(); });
    } else if (raw.Kty == "OKP") {
        if (raw.Crv == "Ed25519") {
            if (raw.D) either([&] { return raw.edPrivateKey(); });
            else either([&] { return raw.edPublicKey(); });
        }
    } else if (raw.Kty.empty()) {
        // kty MUST be present
        return errors::New("go-jose/go-jose: missing json web key type");
    }
    // Symmetric (oct) keys are not ported: they are unsupported here.
    if (err) return err;
    if (!key) {
        // RFC 7517:
        // 5.  JWK Set Format
        // ...
        //     Implementations SHOULD ignore JWKs within a JWK Set that use "kty"
        //     (key type) values that are not understood by them, that are missing
        //     required members, or for which values are out of the supported
        //     ranges.
        return ErrUnsupportedKeyType;
    }

    Key = key;
    KeyID = raw.Kid;
    Algorithm = raw.Alg;
    Use = raw.Use;
    return nullptr;
}

bool JSONWebKey::IsPublic() const {
    object* key = Key.get();
    return dynamic_cast<ecdsa::PublicKey*>(key) || dynamic_cast<rsa::PublicKey*>(key) ||
           dynamic_cast<ed25519::PublicKey*>(key);
}

JSONWebKey JSONWebKey::Public() const {
    if (IsPublic()) return *this;
    JSONWebKey ret = *this;
    object* key = Key.get();
    if (auto* k = dynamic_cast<ecdsa::PrivateKey*>(key)) {
        ret.Key = k->Public();
    } else if (auto* k = dynamic_cast<rsa::PrivateKey*>(key)) {
        ret.Key = k->Public();
    } else if (auto* k = dynamic_cast<ed25519::PrivateKey*>(key)) {
        ret.Key = k->Public();
    } else {
        return JSONWebKey{};  // returning invalid key
    }
    return ret;
}

bool JSONWebKey::Valid() const {
    object* key = Key.get();
    if (!key) return false;
    // Go's nil big integers are zero here, which a key may have: only the
    // pointers and sizes are checked.
    if (auto* k = dynamic_cast<ecdsa::PublicKey*>(key)) return k->Curve != nullptr;
    if (auto* k = dynamic_cast<ecdsa::PrivateKey*>(key)) return k->PublicKey.Curve != nullptr;
    if (auto* k = dynamic_cast<rsa::PublicKey*>(key)) return k->E != 0;
    if (auto* k = dynamic_cast<rsa::PrivateKey*>(key)) return k->PublicKey.E != 0 && k->Primes.size() >= 2;
    if (auto* k = dynamic_cast<ed25519::PublicKey*>(key)) return k->key.size() == 32;
    if (auto* k = dynamic_cast<ed25519::PrivateKey*>(key)) return k->key.size() == 64;
    return false;
}

namespace {

std::pair<std::string, error> ecThumbprintInput(const ecdsa::PublicKey& pub) {
    auto [crv, err] = internal::curveName(pub.Curve);
    if (err) return {"", err};
    int coordLength = internal::curveSize(*pub.Curve);
    auto x = pub.X.Bytes(), y = pub.Y.Bytes();
    if (x.size() > static_cast<size_t>(coordLength) || y.size() > static_cast<size_t>(coordLength)) {
        return {"", errors::New("go-jose/go-jose: invalid elliptic key (too large)")};
    }
    return {std::string(R"({"crv":")") + std::string(crv) + R"(","kty":"EC","x":")" +
                std::string(internal::newFixedSizeBuffer(x, coordLength).base64()) + R"(","y":")" +
                std::string(internal::newFixedSizeBuffer(y, coordLength).base64()) + R"("})",
            nullptr};
}

std::pair<std::string, error> rsaThumbprintInput(const rsa::PublicKey& pub) {
    if (pub.N.Sign() <= 0 || pub.E <= 0) return {"", errors::New("go-jose/go-jose: invalid RSA key")};
    return {std::string(R"({"e":")") + std::string(internal::newBufferFromInt(static_cast<uint64_t>(pub.E)).base64()) +
                R"(","kty":"RSA","n":")" + std::string(internal::newBuffer(pub.N.Bytes()).base64()) + R"("})",
            nullptr};
}

std::pair<std::string, error> edThumbprintInput(std::span<const byte> ed) {
    if (error err = internal::validateEd25519PublicKey(ed)) return {"", err};
    return {std::string(R"({"crv":"Ed25519","kty":"OKP","x":")") +
                std::string(internal::newFixedSizeBuffer(ed, 32).base64()) + R"("})",
            nullptr};
}

}  // namespace

std::pair<slice<byte>, error> JSONWebKey::Thumbprint(crypto::Hash hash) const {
    std::string input;
    error err;
    object* key = Key.get();
    if (auto* k = dynamic_cast<ed25519::PublicKey*>(key)) {
        std::tie(input, err) = edThumbprintInput(k->key);
    } else if (auto* k = dynamic_cast<ecdsa::PublicKey*>(key)) {
        std::tie(input, err) = ecThumbprintInput(*k);
    } else if (auto* k = dynamic_cast<ecdsa::PrivateKey*>(key)) {
        std::tie(input, err) = ecThumbprintInput(k->PublicKey);
    } else if (auto* k = dynamic_cast<rsa::PublicKey*>(key)) {
        std::tie(input, err) = rsaThumbprintInput(*k);
    } else if (auto* k = dynamic_cast<rsa::PrivateKey*>(key)) {
        std::tie(input, err) = rsaThumbprintInput(k->PublicKey);
    } else if (auto* k = dynamic_cast<ed25519::PrivateKey*>(key)) {
        if (k->key.size() != static_cast<size_t>(ed25519::PrivateKeySize)) {
            return {slice<byte>(), errors::New("go-jose/go-jose: invalid Ed25519 private key length")};
        }
        std::tie(input, err) = edThumbprintInput(k->key.Sub(32));
    } else {
        return {slice<byte>(), errors::New("go-jose/go-jose: unknown key type '" + typeOf(Key) + "'")};
    }
    if (err) return {slice<byte>(), err};
    if (!crypto::Available(hash)) return {slice<byte>(), errors::New("go-jose/go-jose: hash function not available")};

    auto h = crypto::New(hash);
    h->Write(as_bytes(input));
    return {h->Sum(slice<byte>()), nullptr};
}

// ---------------------------------------------------------------------------
// JSONWebKeySet

slice<JSONWebKey> JSONWebKeySet::Key(std::string_view kid) const {
    slice<JSONWebKey> keys;
    for (const auto& key : Keys) {
        if (key.KeyID == kid) keys = append(keys, key);
    }
    return keys;
}

error JSONWebKeySet::UnmarshalJSON(std::span<const byte> data) {
    struct raw {
        [[= json::tag("keys")]] std::vector<json::RawMessage> keys;
    } r;
    if (error err = json::Unmarshal(data, &r)) return err;

    slice<JSONWebKey> keys;
    for (const auto& msg : r.keys) {
        JSONWebKey k;
        if (error err = k.UnmarshalJSON(msg.data)) {
            // RFC 7517 section 5: ignore the keys that aren't understood.
            if (errors::Is(err, ErrUnsupportedKeyType)) continue;
            return err;
        }
        keys = append(keys, std::move(k));
    }
    Keys = std::move(keys);
    return nullptr;
}

}  // namespace gopp::jose
