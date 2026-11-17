// The crypto/ecdsa package: ECDSA keys and signatures (FIPS 186-5) on the NIST
// curves, over OpenSSL 3.
//
//     auto [key, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
//     auto digest = hashOf(crypto::Hash::SHA256, msg);
//     auto [sig, err2] = ecdsa::SignASN1(nullptr, *key, digest);
//     bool ok = ecdsa::VerifyASN1(key->PublicKey, digest, sig);
//
// Left out of Go's: custom curves (only P-224, P-256, P-384 and P-521 work:
// Verify is false and the other functions return an error for any other),
// PublicKey.ECDH, Bytes and ParseUncompressedPublicKey, PrivateKey.Equal and
// ECDH, and the FIPS 140 and boring modes. As in Go 1.26, the random reader
// is ignored: OpenSSL's own generator is used, so PrivateKey::Sign with a null
// reader is random, not RFC 6979 deterministic. The fields of a key must not
// change after its first use: an EVP_PKEY built from them is cached.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/math/big.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <memory>
#include <span>
#include <tuple>
#include <utility>
#endif

namespace gopp::ecdsa {

// ecdsa.PublicKey.
struct PublicKey final : crypto::PublicKey {
    ptr<elliptic::Curve> Curve;  // one of the four NIST curves
    big::Int X, Y;               // the coordinates of the point

    // pub.Equal(x): whether x is an ecdsa::PublicKey with the same point,
    // on the same curve (the same singleton).
    bool Equal(const crypto::PublicKey& x) const override;

    crypto::detail::KeyCache cache_;  // internal
};

// ecdsa.PrivateKey.
struct PrivateKey final : crypto::PrivateKey {
    ecdsa::PublicKey PublicKey;  // the public part
    big::Int D;                  // the private scalar

    // priv.Public(): a copy of the public key.
    ptr<crypto::PublicKey> Public() const override;
    // priv.Sign(rand, digest, opts): the ASN.1 signature of digest, which
    // is SignASN1. opts is ignored.
    std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> digest,
                                       const crypto::SignerOpts& opts) override;

    crypto::detail::KeyCache cache_;  // internal
};

// ecdsa.GenerateKey: a new key on c, which must be one of the NIST curves.
// rand is ignored.
std::pair<ptr<PrivateKey>, error> GenerateKey(ptr<elliptic::Curve> c, io::Reader* rand);

// ecdsa.Sign: the signature of hash as the integers r and s. A hash longer
// than the curve order is truncated. rand is ignored.
std::tuple<big::Int, big::Int, error> Sign(io::Reader* rand, const PrivateKey& priv, std::span<const byte> hash);
// ecdsa.Verify: whether (r, s) is a valid signature of hash by pub. False,
// never a failure, for r or s out of 1 to N-1, a point that isn't on the
// curve, and a curve that isn't one of the NIST curves.
bool Verify(const PublicKey& pub, std::span<const byte> hash, const big::Int& r, const big::Int& s);

// ecdsa.SignASN1: the signature of hash as a DER SEQUENCE of r and s.
std::pair<slice<byte>, error> SignASN1(io::Reader* rand, const PrivateKey& priv, std::span<const byte> hash);
// ecdsa.VerifyASN1: whether sig is a valid DER signature of hash by pub.
// Like Go's, it takes the DER encoding of exactly one SEQUENCE of two
// minimal, non-negative INTEGERs and nothing after it.
bool VerifyASN1(const PublicKey& pub, std::span<const byte> hash, std::span<const byte> sig);

}  // namespace gopp::ecdsa
