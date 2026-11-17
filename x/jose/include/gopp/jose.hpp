// x/jose, after github.com/go-jose/go-jose/v4 (v4.1.7): JSON Web Keys
// (RFC 7517), the part that JWS verification and OpenID Connect need.
//
//     jose::JSONWebKeySet set;
//     error err = json::Unmarshal(body, &set);      // skips keys it can't use
//     for (auto& k : set.Key("kid-1")) { ... k.Key ... }
//
// A key's Key is a ptr<rsa::PublicKey>, ptr<ecdsa::PublicKey>,
// ptr<ed25519::PublicKey> or one of their private keys; Go's type switch
// is a dynamic_cast. The keys of untrusted JSON are validated as go-jose
// does, and built in full before they are returned (the OpenSSL keys of
// crypto/rsa and crypto/ecdsa are cached on first use).
//
// Left out of go-jose: symmetric (oct) keys, and X.509 (x5c, x5t,
// x5t#S256, x5u): those members are ignored when unmarshaling, and an oct
// key is jose::ErrUnsupportedKeyType. Also OpaqueSigner. A JWK with
// private parameters unmarshals, as go-jose's does. An EC key that Go
// reports as missing x, y or d (nil) is reported by go++ when it is zero.
#pragma once

#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <span>
#include <string_view>
#include <utility>

namespace gopp::jose {

// jose.SignatureAlgorithm: the "alg" of a JWS. Go's is a string type.
using SignatureAlgorithm = string;

inline constexpr std::string_view RS256 = "RS256";  // RSASSA-PKCS-v1.5 using SHA-256
inline constexpr std::string_view RS384 = "RS384";
inline constexpr std::string_view RS512 = "RS512";
inline constexpr std::string_view PS256 = "PS256";  // RSASSA-PSS using SHA-256 and MGF1-SHA256
inline constexpr std::string_view PS384 = "PS384";
inline constexpr std::string_view PS512 = "PS512";
inline constexpr std::string_view ES256 = "ES256";  // ECDSA using P-256 and SHA-256
inline constexpr std::string_view ES384 = "ES384";
inline constexpr std::string_view ES512 = "ES512";
inline constexpr std::string_view EdDSA = "EdDSA";  // Ed25519
inline constexpr std::string_view HS256 = "HS256";  // HMAC using SHA-256
inline constexpr std::string_view HS384 = "HS384";
inline constexpr std::string_view HS512 = "HS512";

// jose.ErrUnsupportedKeyType: "go-jose/go-jose: unsupported key type/format".
extern const error ErrUnsupportedKeyType;
// jose.ErrUnsupportedEllipticCurve: "go-jose/go-jose: unsupported/unknown elliptic curve".
extern const error ErrUnsupportedEllipticCurve;

// jose.MaxRSAModulusBits: the largest RSA modulus, in bits, that parsing a
// JWK accepts. Change it before any parsing.
extern int MaxRSAModulusBits;

// jose.JSONWebKey: a public or private key in JWK format.
struct JSONWebKey {
    // The key: a ptr<rsa::PublicKey>, ptr<ecdsa::PublicKey>,
    // ptr<ed25519::PublicKey> or ptr<rsa::PrivateKey>,
    // ptr<ecdsa::PrivateKey>, ptr<ed25519::PrivateKey>. The "kty" of the
    // JSON is set from its type.
    ptr<object> Key;
    string KeyID;      // "kid"
    string Algorithm;  // "alg"
    string Use;        // "use"

    // k.MarshalJSON(): the JWK. An error for a key that isn't valid.
    std::pair<slice<byte>, error> MarshalJSON() const;
    // k.UnmarshalJSON(data): reads a JWK, validating the key. An unknown
    // or unsupported "kty" is ErrUnsupportedKeyType; k is unchanged if it
    // fails.
    error UnmarshalJSON(std::span<const byte> data);

    // k.IsPublic(): whether Key is a public key.
    bool IsPublic() const;
    // k.Public(): the key with the public key of Key, or an empty
    // JSONWebKey if Key is not a key of the above types.
    JSONWebKey Public() const;
    // k.Valid(): whether Key has the parameters of its type.
    bool Valid() const;
    // k.Thumbprint(h): the JWK Thumbprint (RFC 7638) of the key.
    std::pair<slice<byte>, error> Thumbprint(crypto::Hash h) const;
};

// jose.JSONWebKeySet: a JWK Set.
struct JSONWebKeySet {
    [[= json::tag("keys")]] slice<JSONWebKey> Keys;

    // s.Key(kid): the keys with the key ID kid.
    slice<JSONWebKey> Key(std::string_view kid) const;
    // Reads a JWK Set. As RFC 7517 section 5 says, keys of an unsupported
    // type (ErrUnsupportedKeyType) are skipped. Other errors fail the set.
    error UnmarshalJSON(std::span<const byte> data);
};

}  // namespace gopp::jose
