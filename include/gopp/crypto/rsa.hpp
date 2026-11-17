// The crypto/rsa package, the signature half of it: RSA keys, and PKCS #1
// v1.5 and PSS signatures (RFC 8017) over OpenSSL 3.
//
//     auto [key, err] = rsa::GenerateKey(nullptr, 2048);
//     auto digest = hashOf(crypto::Hash::SHA256, msg);
//     auto [sig, err2] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, digest);
//     error verr = rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA256, digest, sig);
//
// Left out of Go's: encryption and decryption (EncryptOAEP, EncryptPKCS1v15
// and their Decrypt counterparts), key precomputation (Precomputed, CRTValues,
// Precompute) and the multi-prime and FIPS 140 machinery. As in Go 1.26, the
// random reader of signing is ignored: OpenSSL's own generator is used. The
// 1024 bit minimum of Go is applied by GenerateKey only. The fields of a key
// must not change after its first use: an EVP_PKEY built from them is cached.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/math/big.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <memory>
#include <span>
#include <utility>
#include <vector>
#endif

namespace gopp::rsa {

// rsa.PublicKey.
struct PublicKey final : crypto::PublicKey {
    big::Int N;  // the modulus
    int E = 0;   // the public exponent

    // pub.Size(): the modulus size in bytes, which is the size of a signature.
    int Size() const;
    // pub.Equal(x): whether x is an rsa::PublicKey with the same N and E.
    bool Equal(const crypto::PublicKey& x) const override;

    crypto::detail::KeyCache cache_;  // internal
};

// rsa.PSSOptions.
struct PSSOptions final : crypto::SignerOpts {
    // The length of the salt: PSSSaltLengthAuto, PSSSaltLengthEqualsHash,
    // or a number of bytes. Auto is the longest possible when signing,
    // and found from the signature when verifying.
    int SaltLength = 0;
    // The hash function the digest was made with. If zero, the hash
    // argument of SignPSS and VerifyPSS is used.
    crypto::Hash Hash{};

    crypto::Hash HashFunc() const override { return Hash; }
};

inline constexpr int PSSSaltLengthAuto = 0;
inline constexpr int PSSSaltLengthEqualsHash = -1;

// rsa.PrivateKey. Only keys of two primes.
struct PrivateKey final : crypto::PrivateKey {
    rsa::PublicKey PublicKey;           // the public part
    big::Int D;                         // the private exponent
    std::vector<big::Int> Primes;       // the prime factors of N, at least 2

    // priv.Public(): a copy of the public key.
    ptr<crypto::PublicKey> Public() const override;
    // priv.Sign(rand, digest, opts): a PSS signature if opts is a
    // PSSOptions, otherwise a PKCS #1 v1.5 signature with opts.HashFunc().
    std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> digest,
                                       const crypto::SignerOpts& opts) override;
    // priv.Validate(): nil if the key is consistent: the factors multiply
    // to N, and D is the inverse of E.
    error Validate() const;

    crypto::detail::KeyCache cache_;  // internal
};

// rsa.GenerateKey: a key of bits bits, at least 1024. random is ignored.
std::pair<ptr<PrivateKey>, error> GenerateKey(io::Reader* random, int bits);

// rsa.SignPKCS1v15: the RSASSA-PKCS1-v1_5 signature of hashed, the digest
// made with hash. If hash is zero, hashed is signed as it is.
std::pair<slice<byte>, error> SignPKCS1v15(io::Reader* random, const PrivateKey& priv, crypto::Hash hash,
                                           std::span<const byte> hashed);
// rsa.VerifyPKCS1v15: nil if sig is a valid signature of hashed, and
// ErrVerification if not.
error VerifyPKCS1v15(const PublicKey& pub, crypto::Hash hash, std::span<const byte> hashed,
                     std::span<const byte> sig);

// rsa.SignPSS: the RSASSA-PSS signature of digest. opts may be nullptr:
// the salt is of the longest length then, and hash is used.
std::pair<slice<byte>, error> SignPSS(io::Reader* rand, const PrivateKey& priv, crypto::Hash hash,
                                      std::span<const byte> digest, const PSSOptions* opts);
// rsa.VerifyPSS: nil if sig is a valid PSS signature of digest, and
// ErrVerification if not.
error VerifyPSS(const PublicKey& pub, crypto::Hash hash, std::span<const byte> digest, std::span<const byte> sig,
                const PSSOptions* opts);

// rsa.ErrVerification: "crypto/rsa: verification error".
extern const error ErrVerification;

}  // namespace gopp::rsa
