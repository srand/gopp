// crypto/rsa on OpenSSL 3's EVP_PKEY: keys, PKCS #1 v1.5 and PSS signatures.
#include <gopp/crypto/rsa.hpp>
#include <gopp/sync.hpp>

#include "crypto/pkey.hpp"
#include "crypto/rsa_internal.hpp"

#include <openssl/rsa.h>

#include <atomic>

namespace gopp::rsa {

const error ErrVerification = errors::New("crypto/rsa: verification error");

namespace {

constexpr const char* pkg = "crypto/rsa";

// The key as an EVP_PKEY, built on first use.
std::pair<EVP_PKEY*, error> publicEvp(const PublicKey& pub) {
    return pkey::cached(pub.cache_, [&]() -> std::pair<pkey::Key, error> {
        if (pub.N.Sign() <= 0) {
            return {pkey::Key(), errors::New("crypto/rsa: missing public modulus")};
        }
        if (pub.E < 2) {
            return {pkey::Key(), errors::New("crypto/rsa: public exponent too small or negative")};
        }
        big::Int e;
        e.SetInt64(pub.E);
        pkey::ParamBuilder b;
        b.add(OSSL_PKEY_PARAM_RSA_N, pub.N);
        b.add(OSSL_PKEY_PARAM_RSA_E, e);
        auto [k, err] = pkey::fromParams("RSA", EVP_PKEY_PUBLIC_KEY, b, pkg);
        return {std::move(k), err};
    });
}

std::pair<EVP_PKEY*, error> privateEvp(const PrivateKey& priv) {
    return pkey::cached(priv.cache_, [&]() -> std::pair<pkey::Key, error> {
        const PublicKey& pub = priv.PublicKey;
        if (pub.N.Sign() <= 0) {
            return {pkey::Key(), errors::New("crypto/rsa: missing public modulus")};
        }
        if (pub.E < 2) {
            return {pkey::Key(), errors::New("crypto/rsa: public exponent too small or negative")};
        }
        if (priv.Primes.size() != 2) {
            return {pkey::Key(), errors::New("crypto/rsa: only keys of two primes are supported")};
        }
        const BIGNUM* d = pkey::bn(priv.D);
        const BIGNUM* p = pkey::bn(priv.Primes[0]);
        const BIGNUM* q = pkey::bn(priv.Primes[1]);
        // The CRT values, which make signing several times faster.
        BN_CTX* ctx = BN_CTX_new();
        pkey::BN p1(BN_new()), q1(BN_new()), dp(BN_new()), dq(BN_new());
        pkey::BN qinv;
        bool ok = ctx && p1 && q1 && dp && dq && BN_sub(p1.get(), p, BN_value_one()) == 1 &&
                  BN_sub(q1.get(), q, BN_value_one()) == 1 && !BN_is_zero(p1.get()) && !BN_is_zero(q1.get()) &&
                  BN_mod(dp.get(), d, p1.get(), ctx) == 1 && BN_mod(dq.get(), d, q1.get(), ctx) == 1;
        if (ok) qinv.reset(BN_mod_inverse(nullptr, q, p, ctx));
        BN_CTX_free(ctx);
        if (!ok || !qinv) {
            ERR_clear_error();
            return {pkey::Key(), errors::New("crypto/rsa: invalid private key")};
        }
        big::Int e;
        e.SetInt64(pub.E);
        pkey::ParamBuilder b;
        b.add(OSSL_PKEY_PARAM_RSA_N, pub.N);
        b.add(OSSL_PKEY_PARAM_RSA_E, e);
        b.add(OSSL_PKEY_PARAM_RSA_D, d);
        b.add(OSSL_PKEY_PARAM_RSA_FACTOR1, p);
        b.add(OSSL_PKEY_PARAM_RSA_FACTOR2, q);
        b.add(OSSL_PKEY_PARAM_RSA_EXPONENT1, dp.get());
        b.add(OSSL_PKEY_PARAM_RSA_EXPONENT2, dq.get());
        b.add(OSSL_PKEY_PARAM_RSA_COEFFICIENT1, qinv.get());
        auto [k, err] = pkey::fromParams("RSA", EVP_PKEY_KEYPAIR, b, pkg);
        return {std::move(k), err};
    });
}

enum class Padding { PKCS1v15, PSS };

// Sets up ctx for signing or verifying with the padding and hash. salt is
// OpenSSL's salt length for PSS.
error configure(EVP_PKEY_CTX* ctx, Padding pad, crypto::Hash hash, int salt) {
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, pad == Padding::PSS ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) <= 0)
        return pkey::openSSLError(pkg, "setting the padding");
    if (hash != crypto::Hash{}) {
        const EVP_MD* md = EVP_get_digestbyname(pkey::mdName(hash));
        if (!md || EVP_PKEY_CTX_set_signature_md(ctx, md) <= 0) return pkey::openSSLError(pkg, "setting the hash");
    }
    if (pad == Padding::PSS && EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, salt) <= 0)
        return pkey::openSSLError(pkg, "setting the salt length");
    return nullptr;
}

// OpenSSL's salt length for ours, and whether it is valid.
bool saltFor(int saltLength, bool verifying, int& out) {
    if (saltLength < PSSSaltLengthEqualsHash) return false;
    if (saltLength == PSSSaltLengthAuto)
        out = verifying ? RSA_PSS_SALTLEN_AUTO : RSA_PSS_SALTLEN_MAX;
    else if (saltLength == PSSSaltLengthEqualsHash)
        out = RSA_PSS_SALTLEN_DIGEST;
    else
        out = saltLength;
    return true;
}

std::pair<slice<byte>, error> sign(const PrivateKey& priv, Padding pad, crypto::Hash hash, int salt,
                                   std::span<const byte> digest) {
    auto [key, kerr] = privateEvp(priv);
    if (kerr) return {slice<byte>(), kerr};
    pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx || EVP_PKEY_sign_init(ctx.get()) != 1) return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    if (error err = configure(ctx.get(), pad, hash, salt)) return {slice<byte>(), err};
    size_t n = 0;
    if (EVP_PKEY_sign(ctx.get(), nullptr, &n, digest.data(), digest.size()) != 1)
        return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    std::vector<unsigned char> sig(n);
    if (EVP_PKEY_sign(ctx.get(), sig.data(), &n, digest.data(), digest.size()) != 1)
        return {slice<byte>(), pkey::openSSLError(pkg, "signing")};
    return {appendSpan(slice<byte>(), std::span<const byte>(reinterpret_cast<const byte*>(sig.data()), n)), nullptr};
}

error verify(const PublicKey& pub, Padding pad, crypto::Hash hash, int salt, std::span<const byte> digest,
             std::span<const byte> sig) {
    // As Go does: OpenSSL accepts a PSS signature with its leading zeros cut.
    if (sig.size() != static_cast<size_t>(pub.Size())) return ErrVerification;
    auto [key, kerr] = publicEvp(pub);
    if (kerr) return kerr;
    pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx || EVP_PKEY_verify_init(ctx.get()) != 1) return pkey::openSSLError(pkg, "verifying");
    if (error err = configure(ctx.get(), pad, hash, salt)) return err;
    int r = EVP_PKEY_verify(ctx.get(), sig.data(), sig.size(), digest.data(), digest.size());
    ERR_clear_error();
    return r == 1 ? nullptr : ErrVerification;
}

std::atomic<int> allowSmall{0};

error checkKeySize(int bits) {
    if (bits >= 1024 || allowSmall.load() > 0) return nullptr;
    return errors::New(string("crypto/rsa: " + std::to_string(bits) +
                              "-bit keys are insecure (see https://go.dev/pkg/crypto/rsa#hdr-Minimum_key_size)"));
}

error checkPublicKeySize(const PublicKey& k) {
    if (k.N.Sign() <= 0) return errors::New("crypto/rsa: missing public modulus");
    return checkKeySize(k.N.BitLen());
}

}  // namespace

int PublicKey::Size() const { return (N.BitLen() + 7) / 8; }

bool PublicKey::Equal(const crypto::PublicKey& x) const {
    auto xx = dynamic_cast<const rsa::PublicKey*>(&x);
    return xx && N.Cmp(xx->N) == 0 && E == xx->E;
}

ptr<crypto::PublicKey> PrivateKey::Public() const { return make_ptr<rsa::PublicKey>(PublicKey); }

std::pair<slice<byte>, error> PrivateKey::Sign(io::Reader* rand, std::span<const byte> digest,
                                               const crypto::SignerOpts& opts) {
    if (auto pss = dynamic_cast<const PSSOptions*>(&opts)) return SignPSS(rand, *this, pss->Hash, digest, pss);
    return SignPKCS1v15(rand, *this, opts.HashFunc(), digest);
}

error PrivateKey::Validate() const {
    auto [key, err] = privateEvp(*this);
    if (err) return err;
    pkey::Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx || EVP_PKEY_check(ctx.get()) != 1) return pkey::openSSLError(pkg, "validating the key");
    return nullptr;
}

std::pair<ptr<PrivateKey>, error> GenerateKey(io::Reader*, int bits) {
    if (error err = checkKeySize(bits)) return {nullptr, err};
    pkey::Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(bits)));
    if (!key) return {nullptr, pkey::openSSLError(pkg, "generating the key")};
    auto priv = make_ptr<PrivateKey>();
    auto [n, okN] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_RSA_N);
    auto [e, okE] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_RSA_E);
    auto [d, okD] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_RSA_D);
    auto [p, okP] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR1);
    auto [q, okQ] = pkey::getInt(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR2);
    if (!(okN && okE && okD && okP && okQ) || !e.IsInt64()) return {nullptr, pkey::openSSLError(pkg, "reading the key")};
    priv->PublicKey.N = std::move(n);
    priv->PublicKey.E = static_cast<int>(e.Int64());
    priv->D = std::move(d);
    priv->Primes.push_back(std::move(p));
    priv->Primes.push_back(std::move(q));
    return {priv, nullptr};
}

std::pair<slice<byte>, error> SignPKCS1v15(io::Reader*, const PrivateKey& priv, crypto::Hash hash,
                                           std::span<const byte> hashed) {
    if (hash != crypto::Hash{} && static_cast<isize>(hashed.size()) != crypto::Size(hash))
        return {slice<byte>(), errors::New("crypto/rsa: input must be hashed message")};
    if (error err = checkPublicKeySize(priv.PublicKey)) return {slice<byte>(), err};
    return sign(priv, Padding::PKCS1v15, hash, 0, hashed);
}

error VerifyPKCS1v15(const PublicKey& pub, crypto::Hash hash, std::span<const byte> hashed,
                     std::span<const byte> sig) {
    if (error err = checkPublicKeySize(pub)) return err;
    if (hash != crypto::Hash{} && static_cast<isize>(hashed.size()) != crypto::Size(hash)) return ErrVerification;
    return verify(pub, Padding::PKCS1v15, hash, 0, hashed, sig);
}

std::pair<slice<byte>, error> SignPSS(io::Reader*, const PrivateKey& priv, crypto::Hash hash,
                                      std::span<const byte> digest, const PSSOptions* opts) {
    if (error err = checkPublicKeySize(priv.PublicKey)) return {slice<byte>(), err};
    if (opts && opts->Hash != crypto::Hash{}) hash = opts->Hash;
    int salt = 0;
    if (!saltFor(opts ? opts->SaltLength : PSSSaltLengthAuto, false, salt))
        return {slice<byte>(), errors::New("crypto/rsa: invalid PSS salt length")};
    crypto::Size(hash);  // a hash that isn't one panics, as Go's Hash.New does
    return sign(priv, Padding::PSS, hash, salt, digest);
}

error VerifyPSS(const PublicKey& pub, crypto::Hash hash, std::span<const byte> digest, std::span<const byte> sig,
                const PSSOptions* opts) {
    if (error err = checkPublicKeySize(pub)) return err;
    // Unlike SignPSS, opts->Hash is not used: Go's VerifyPSS ignores it.
    int salt = 0;
    if (!saltFor(opts ? opts->SaltLength : PSSSaltLengthAuto, true, salt)) return ErrVerification;
    crypto::Size(hash);
    return verify(pub, Padding::PSS, hash, salt, digest, sig);
}

namespace detail {

AllowSmallKeys::AllowSmallKeys() { allowSmall.fetch_add(1); }
AllowSmallKeys::~AllowSmallKeys() { allowSmall.fetch_sub(1); }

ptr<PrivateKey> fromComponents(std::string_view n, int e, std::string_view d, std::string_view p,
                               std::string_view q) {
    auto priv = make_ptr<PrivateKey>();
    big::Int p0, q0;
    priv->PublicKey.N.SetString(n, 16);
    priv->PublicKey.E = e;
    priv->D.SetString(d, 16);
    p0.SetString(p, 16);
    q0.SetString(q, 16);
    priv->Primes.push_back(std::move(p0));
    priv->Primes.push_back(std::move(q0));
    return priv;
}

}  // namespace detail

}  // namespace gopp::rsa
