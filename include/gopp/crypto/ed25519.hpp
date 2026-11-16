// The crypto/ed25519 package: Ed25519 signatures (RFC 8032) over OpenSSL 3.
//
//     auto [pub, priv, err] = ed25519::GenerateKey(nullptr);
//     auto sig = ed25519::Sign(*priv, message);
//     bool ok = ed25519::Verify(*pub, message, sig);
//
// Go's PublicKey and PrivateKey are byte slices; here they are structs
// holding the slice, as that is how they implement crypto::PublicKey and
// crypto::Signer. Left out of Go's: Options, VerifyWithOptions, and so
// Ed25519ph (a SHA-512 SignerOpts) and Ed25519ctx; Sign returns an error for
// them. As Go does, Verify, Sign and NewKeyFromSeed panic on a public key,
// private key or seed of the wrong length; a wrong signature length is
// just false.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <span>
#include <tuple>
#include <utility>
#endif

namespace gopp::ed25519 {

inline constexpr isize PublicKeySize = 32;
inline constexpr isize PrivateKeySize = 64;
inline constexpr isize SignatureSize = 64;
inline constexpr isize SeedSize = 32;

// ed25519.PublicKey: 32 bytes.
struct PublicKey final : crypto::PublicKey {
    slice<byte> key;

    // pub.Equal(x): whether x is an ed25519::PublicKey of the same bytes.
    bool Equal(const crypto::PublicKey& x) const override;
};

// ed25519.PrivateKey: 64 bytes, the seed followed by the public key.
struct PrivateKey final : crypto::PrivateKey {
    slice<byte> key;

    // priv.Public(): an ed25519::PublicKey, a copy of the last 32 bytes.
    ptr<crypto::PublicKey> Public() const override;
    // priv.Seed(): a copy of the first 32 bytes.
    slice<byte> Seed() const;
    // priv.Sign(rand, message, opts): Sign(priv, message), if
    // opts.HashFunc() is zero; an error for Ed25519ph.
    std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> message,
                                       const crypto::SignerOpts& opts) override;
};

// ed25519.GenerateKey: a new key pair from a seed read from rand, or from
// OpenSSL's generator if rand is null.
std::tuple<ptr<PublicKey>, ptr<PrivateKey>, error> GenerateKey(io::Reader* rand);
// ed25519.NewKeyFromSeed: the private key of a 32 byte seed. Panics on any
// other length.
ptr<PrivateKey> NewKeyFromSeed(std::span<const byte> seed);
// ed25519.Sign: the 64 byte signature of message. Panics if priv is not a
// valid private key.
slice<byte> Sign(const PrivateKey& priv, std::span<const byte> message);
// ed25519.Verify: whether sig is a valid signature of message by pub. False
// for a signature that is not 64 bytes, has a non-canonical S, or whose
// public key is not a point of the curve. Panics if pub is not 32 bytes.
bool Verify(const PublicKey& pub, std::span<const byte> message, std::span<const byte> sig);

}  // namespace gopp::ed25519
