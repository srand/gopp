// crypto/ed25519 on OpenSSL 3's raw Ed25519 keys and one-shot signing.
#include <gopp/crypto/ed25519.hpp>

#include "crypto/pkey.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace gopp::ed25519 {

namespace {

const unsigned char* uc(std::span<const byte> s) { return reinterpret_cast<const unsigned char*>(s.data()); }

struct mdCtxDeleter {
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, mdCtxDeleter>;

slice<byte> copyOf(const unsigned char* p, size_t n) {
    return appendSpan(slice<byte>(), std::span<const byte>(reinterpret_cast<const byte*>(p), n));
}

// The 32 bytes of the public key of a seed.
std::pair<pkey::Key, slice<byte>> fromSeed(std::span<const byte> seed) {
    pkey::Key key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, uc(seed), seed.size()));
    unsigned char pub[PublicKeySize];
    size_t n = sizeof pub;
    if (!key || EVP_PKEY_get_raw_public_key(key.get(), pub, &n) != 1 || n != sizeof pub) {
        ERR_clear_error();
        panic("ed25519: could not derive the public key");
    }
    return {std::move(key), copyOf(pub, n)};
}

}  // namespace

bool PublicKey::Equal(const crypto::PublicKey& x) const {
    auto xx = dynamic_cast<const ed25519::PublicKey*>(&x);
    if (!xx || key.len() != xx->key.len()) return false;
    return key.len() == 0 || CRYPTO_memcmp(key.data(), xx->key.data(), static_cast<size_t>(key.len())) == 0;
}

ptr<crypto::PublicKey> PrivateKey::Public() const {
    auto pub = make_ptr<ed25519::PublicKey>();
    // As Go's priv[32:], which panics on a key that is too short.
    pub->key = copyOf(reinterpret_cast<const unsigned char*>(key.Sub(32, PrivateKeySize).data()), PublicKeySize);
    return pub;
}

slice<byte> PrivateKey::Seed() const {
    return copyOf(reinterpret_cast<const unsigned char*>(key.Sub(0, SeedSize).data()), SeedSize);
}

std::pair<slice<byte>, error> PrivateKey::Sign(io::Reader*, std::span<const byte> message,
                                               const crypto::SignerOpts& opts) {
    if (opts.HashFunc() != crypto::Hash{})
        return {slice<byte>(),
                errors::New("ed25519: expected opts.HashFunc() zero (unhashed message, for standard Ed25519); "
                            "Ed25519ph is not supported")};
    return {ed25519::Sign(*this, message), nullptr};
}

std::tuple<ptr<PublicKey>, ptr<PrivateKey>, error> GenerateKey(io::Reader* rand) {
    unsigned char seed[SeedSize];
    if (rand) {
        auto [n, err] = io::ReadFull(*rand, std::span<byte>(reinterpret_cast<byte*>(seed), sizeof seed));
        if (err) return {nullptr, nullptr, err};
    } else if (RAND_bytes(seed, sizeof seed) != 1) {
        ERR_clear_error();
        return {nullptr, nullptr, errors::New("crypto/ed25519: could not read random bytes")};
    }
    auto priv = NewKeyFromSeed(std::span<const byte>(reinterpret_cast<const byte*>(seed), sizeof seed));
    OPENSSL_cleanse(seed, sizeof seed);
    auto pub = make_ptr<PublicKey>();
    pub->key = copyOf(reinterpret_cast<const unsigned char*>(priv->key.Sub(32, PrivateKeySize).data()), PublicKeySize);
    return {pub, priv, nullptr};
}

ptr<PrivateKey> NewKeyFromSeed(std::span<const byte> seed) {
    if (static_cast<isize>(seed.size()) != SeedSize)
        panic("ed25519: bad seed length: " + std::to_string(seed.size()));
    auto [key, pub] = fromSeed(seed);
    auto priv = make_ptr<PrivateKey>();
    auto k = slice<byte>::make(PrivateKeySize);
    std::memcpy(k.data(), seed.data(), SeedSize);
    std::memcpy(k.data() + SeedSize, pub.data(), PublicKeySize);
    priv->key = k;
    return priv;
}

slice<byte> Sign(const PrivateKey& priv, std::span<const byte> message) {
    if (priv.key.len() != PrivateKeySize)
        panic("ed25519: bad private key: bad private key length: " + std::to_string(priv.key.len()));
    auto [key, pub] = fromSeed(std::span<const byte>(priv.key.data(), SeedSize));
    // The public half has to be the seed's, as in Go's NewPrivateKey.
    if (CRYPTO_memcmp(pub.data(), priv.key.data() + SeedSize, PublicKeySize) != 0)
        panic("ed25519: bad private key: bad private key");
    MdCtx ctx(EVP_MD_CTX_new());
    unsigned char sig[SignatureSize];
    size_t n = sizeof sig;
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(ctx.get(), sig, &n, uc(message), message.size()) != 1 || n != sizeof sig) {
        ERR_clear_error();
        panic("ed25519: signing failed");
    }
    return copyOf(sig, n);
}

bool Verify(const PublicKey& pub, std::span<const byte> message, std::span<const byte> sig) {
    if (pub.key.len() != PublicKeySize) panic("ed25519: bad public key length: " + std::to_string(pub.key.len()));
    if (static_cast<isize>(sig.size()) != SignatureSize) return false;
    pkey::Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              reinterpret_cast<const unsigned char*>(pub.key.data()), PublicKeySize));
    MdCtx ctx(EVP_MD_CTX_new());
    bool ok = key && ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
              EVP_DigestVerify(ctx.get(), uc(sig), sig.size(), uc(message), message.size()) == 1;
    ERR_clear_error();
    return ok;
}

}  // namespace gopp::ed25519
