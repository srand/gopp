// x/jose's internals: go-jose's encoding.go (the base64url byteBuffer and
// the helpers that make one), and jwk.go's rawJSONWebKey with its
// conversions to and from keys.
#pragma once

#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/jose.hpp>
#include <gopp/math/big.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace gopp::jose::internal {

// stripWhitespace: data without its spaces, newlines and the like.
std::string stripWhitespace(std::string_view data);

// byteBuffer: bytes that are serialized as unpadded base64url. Go's is
// held by pointer, where a nil one is an empty slice; here that is an
// optional.
struct byteBuffer {
    slice<byte> data;

    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> data);

    string base64() const;
    std::span<const byte> bytes() const { return data; }
    big::Int bigInt() const;
};

// newBuffer: a byteBuffer of a copy of data.
byteBuffer newBuffer(std::span<const byte> data);
// newFixedSizeBuffer: data, with zero bytes in front up to length bytes.
// Panics if data is longer.
byteBuffer newFixedSizeBuffer(std::span<const byte> data, int length);
// newBufferFromInt: the big-endian bytes of num, without leading zeros.
byteBuffer newBufferFromInt(uint64_t num);

// curveName: "P-256", "P-384" or "P-521"; ErrUnsupportedEllipticCurve for
// any other curve, and for none.
std::pair<string, error> curveName(const ptr<elliptic::Curve>& crv);
// curveSize: the size in bytes of a coordinate of the curve.
int curveSize(const elliptic::Curve& crv);
// dSize: the size in bytes of the "d" member of an EC private key.
int dSize(const elliptic::Curve& crv);

// rawJSONWebKey: a public or private key in JWK format, for parsing and
// serializing. Without go-jose's k, dp, dq, qi (a private RSA key has no
// precomputed values here) and X.509 members, which parsing ignores. For RSA and ECDSA, D present means a private key.
struct rawJSONWebKey {
    [[= json::tag("use,omitempty")]] string Use;
    [[= json::tag("kty,omitempty")]] string Kty;
    [[= json::tag("kid,omitempty")]] string Kid;
    [[= json::tag("crv,omitempty")]] string Crv;
    [[= json::tag("alg,omitempty")]] string Alg;
    [[= json::tag("x,omitempty")]] std::optional<byteBuffer> X;
    [[= json::tag("y,omitempty")]] std::optional<byteBuffer> Y;
    [[= json::tag("n,omitempty")]] std::optional<byteBuffer> N;
    [[= json::tag("e,omitempty")]] std::optional<byteBuffer> E;
    [[= json::tag("d,omitempty")]] std::optional<byteBuffer> D;
    [[= json::tag("p,omitempty")]] std::optional<byteBuffer> P;
    [[= json::tag("q,omitempty")]] std::optional<byteBuffer> Q;

    std::pair<ptr<rsa::PublicKey>, error> rsaPublicKey() const;
    std::pair<ptr<rsa::PrivateKey>, error> rsaPrivateKey() const;
    std::pair<ptr<ecdsa::PublicKey>, error> ecPublicKey() const;
    std::pair<ptr<ecdsa::PrivateKey>, error> ecPrivateKey() const;
    std::pair<ptr<ed25519::PublicKey>, error> edPublicKey() const;
    std::pair<ptr<ed25519::PrivateKey>, error> edPrivateKey() const;
};

std::pair<rawJSONWebKey, error> fromEdPublicKey(const ed25519::PublicKey& pub);
rawJSONWebKey fromRsaPublicKey(const rsa::PublicKey& pub);
std::pair<rawJSONWebKey, error> fromEcPublicKey(const ecdsa::PublicKey& pub);
std::pair<rawJSONWebKey, error> fromEdPrivateKey(const ed25519::PrivateKey& ed);
std::pair<rawJSONWebKey, error> fromRsaPrivateKey(const rsa::PrivateKey& rsa);
std::pair<rawJSONWebKey, error> fromEcPrivateKey(const ecdsa::PrivateKey& ec);

// validateEd25519PublicKey: an error unless the key is 32 bytes and not
// one of the low-order encodings that go-jose refuses.
error validateEd25519PublicKey(std::span<const byte> publicKey);

}  // namespace gopp::jose::internal
