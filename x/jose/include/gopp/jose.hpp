// x/jose, after github.com/go-jose/go-jose/v4 (v4.1.7): JSON Web Keys
// (RFC 7517) and JSON Web Signatures (RFC 7515), the part that OpenID
// Connect needs. JSON Web Tokens are <gopp/jose/jwt.hpp>.
//
//     jose::JSONWebKeySet set;
//     error err = json::Unmarshal(body, &set);      // skips keys it can't use
//     for (auto& k : set.Key("kid-1")) { ... k.Key ... }
//
//     auto [jws, err] = jose::ParseSigned(token, std::array{jose::RS256});
//     auto [payload, err2] = jws->Verify(set);      // the key of the "kid"
//
//     auto [signer, err] = jose::NewSigner({jose::ES256, privateKey}, nullptr);
//     auto [obj, err2] = signer->Sign(payload);
//     auto [compact, err3] = obj->CompactSerialize();
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
//
// Of JWS, only the compact serialization: ParseSigned of the JSON
// serializations is ErrNotSupported, as OpenID Connect's tokens are
// compact. So there is one signature, and no unprotected header. Also
// left out: the HMAC algorithms (there are no symmetric keys: NewSigner
// fails with ErrUnsupportedAlgorithm, and no key verifies them), detached
// payloads, NewMultiSigner, NonceSource, the x5c header (it is checked to
// be a list of strings, and dropped) and Header.Certificates.
// Verification is as strict as go-jose's on untrusted input: the "alg" of
// the protected header is checked against the allowed algorithms before
// anything else; the key must fit it (an RSA key for RS* and PS*, the
// curve's ECDSA key for ES256, ES384 and ES512, Ed25519 for EdDSA); an
// ECDSA signature is r and s of the curve's size; and an embedded "jwk"
// is never used to verify unless passed as the key. Where go-jose panics
// (an Ed25519 key that isn't 32 bytes, a message without signatures),
// go++ fails.
//
// As go-jose parses with its own case-sensitive json, all JSON here (JWKs
// and JWK Sets, JWS headers, and JWT claims, including the caller's claims
// structs) is decoded with json::UnmarshalCaseSensitive: a member sets a
// field only by its exact name, so "KTY" is not "kty".
#pragma once

#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <map>
#include <optional>
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

// jose.JSONWebKey: a public or private key in JWK format. An object, so
// that it can be a SigningKey's or a verification key.
struct JSONWebKey : object {
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

// jose.JSONWebKeySet: a JWK Set. An object, so that it can be a
// verification key.
struct JSONWebKeySet : object {
    [[= json::tag("keys")]] slice<JSONWebKey> Keys;

    // s.Key(kid): the keys with the key ID kid.
    slice<JSONWebKey> Key(std::string_view kid) const;
    // Reads a JWK Set. As RFC 7517 section 5 says, keys of an unsupported
    // type (ErrUnsupportedKeyType) are skipped. Other errors fail the set.
    error UnmarshalJSON(std::span<const byte> data);
};

// ---------------------------------------------------------------------------
// JWS

// jose.ErrCryptoFailure: "go-jose/go-jose: error in cryptographic
// primitive"; the error of a signature that doesn't verify.
extern const error ErrCryptoFailure;
// jose.ErrUnsupportedAlgorithm: "go-jose/go-jose: unknown/unsupported algorithm".
extern const error ErrUnsupportedAlgorithm;
// jose.ErrNotSupported: "go-jose/go-jose: compact serialization not
// supported for object"; also ParseSigned's for a JSON serialization.
extern const error ErrNotSupported;
// jose.ErrUnprotectedNonce: "go-jose/go-jose: Nonce parameter included in
// unprotected header".
extern const error ErrUnprotectedNonce;
// jose.ErrUnsupportedCriticalHeader: a "crit" header that isn't
// supported, or isn't protected.
extern const error ErrUnsupportedCriticalHeader;
// jose.ErrNotPublic: "go-jose/go-jose: public key was unexpectedly not public".
extern const error ErrNotPublic;
// jose.ErrJWKSKidNotFound: verifying with a JWK Set that has no key of the
// message's "kid", or a message without one.
extern const error ErrJWKSKidNotFound;

// jose.ErrUnexpectedSignatureAlgorithm: the "alg" of a message is not one
// of the allowed algorithms.
struct ErrUnexpectedSignatureAlgorithm final : errorInterface {
    SignatureAlgorithm Got;                // the algorithm of the message
    slice<SignatureAlgorithm> expected;    // the allowed ones

    // `unexpected signature algorithm "HS256"; expected ["RS256"]`
    string Error() const override;
};

namespace detail {
// go-jose's rawHeader: the members of a header, as JSON. A member that is
// JSON null is an empty optional, as Go's nil *json.RawMessage.
using rawHeader = std::map<string, std::optional<json::RawMessage>, std::less<>>;
}  // namespace detail

// jose.Header: the members of a JWS header that go-jose knows, and the
// others in ExtraHeaders (as json::Value, Go's interface{}).
struct Header {
    string KeyID;                         // "kid"
    ptr<jose::JSONWebKey> JSONWebKey;     // "jwk": a public key, untrusted
    string Algorithm;                     // "alg"
    string Nonce;                         // "nonce"
    std::map<string, json::Value> ExtraHeaders;
};

// jose.Signature: a signature over the payload and the protected header.
struct Signature {
    // Merged header fields. Contains both protected and unprotected header
    // values. Prefer using Protected and Unprotected fields instead of
    // this. Values in this header may or may not have been signed and in
    // general should not be trusted.
    jose::Header Header;
    // The protected header: its values are signed.
    jose::Header Protected;
    // The unprotected header: always empty in a compact message.
    jose::Header Unprotected;
    // The signature: Go's Signature, as a member can't have its struct's name.
    slice<byte> signature;

    std::optional<detail::rawHeader> protected_;          // internal: Go's protected
    std::optional<detail::rawHeader> header_;             // internal: Go's header
    std::optional<slice<byte>> originalProtected_;        // internal: the protected header as parsed
};

// jose.JSONWebSignature: a signed message, parsed or made by a Signer.
class JSONWebSignature : public object {
public:
    // Signatures attached to this object. Be careful about accessing these
    // directly, prefer to use Verify() to ensure that the data you're
    // getting is verified.
    slice<Signature> Signatures;

    // obj.Verify(key): the payload, if its one signature verifies with
    // key: an rsa::PublicKey, ecdsa::PublicKey or ed25519::PublicKey, a
    // JSONWebKey holding one, or a JSONWebKeySet, whose first key of the
    // message's "kid" is used. Unsupported key types are
    // ErrUnsupportedKeyType, and a signature that doesn't verify is
    // ErrCryptoFailure. Be careful when verifying signatures based on
    // embedded JWKs inside the payload header. You cannot assume that the
    // key received in a payload is trusted.
    std::pair<slice<byte>, error> Verify(const object& verificationKey) const;
    std::pair<slice<byte>, error> Verify(const JSONWebKey& k) const;
    // obj.UnsafePayloadWithoutVerification(): the payload, which cannot be
    // trusted.
    slice<byte> UnsafePayloadWithoutVerification() const;
    // obj.CompactSerialize(): the compact serialization, or
    // ErrNotSupported if there isn't exactly one signature with only a
    // protected header.
    std::pair<string, error> CompactSerialize() const;

    slice<byte> payload;  // internal
};

// jose.ParseSigned: parses a message in JWS Compact Serialization, after
// stripping its white space (unicode::IsSpace). Fails unless its
// algorithm is one of allowedAlgs, which must not be empty. The signature
// is not verified: call Verify. A JSON serialization (the input starts
// with '{') is ErrNotSupported.
std::pair<ptr<JSONWebSignature>, error> ParseSigned(std::string_view s,
                                                    std::span<const std::string_view> allowedAlgs);
// jose.ParseSignedCompact: ParseSigned of a compact message as it is.
std::pair<ptr<JSONWebSignature>, error> ParseSignedCompact(std::string_view s,
                                                           std::span<const std::string_view> allowedAlgs);

// jose.SigningKey: an algorithm and its key, a ptr<rsa::PrivateKey>,
// ptr<ecdsa::PrivateKey>, ptr<ed25519::PrivateKey>, or a ptr<JSONWebKey>
// holding one (its "kid" goes in the header).
struct SigningKey {
    std::string_view Algorithm;
    ptr<object> Key;
};

// jose.SignerOptions: options of a Signer.
struct SignerOptions {
    // Additional members of the protected header.
    std::map<string, json::Value> ExtraHeaders;
    // Whether to embed the public key ("jwk") instead of its "kid".
    bool EmbedJWK = false;

    // so.WithType(t): adds a "typ" header.
    SignerOptions& WithType(std::string_view t);
    // so.WithHeader(k, v): adds an extra header.
    SignerOptions& WithHeader(std::string_view k, json::Value v);

    friend bool operator==(const SignerOptions&, const SignerOptions&) = default;
};

// jose.Signer: signs payloads.
struct Signer : virtual object {
    virtual std::pair<ptr<JSONWebSignature>, error> Sign(std::span<const byte> payload) = 0;
    virtual SignerOptions Options() const = 0;
};

// jose.NewSigner: a Signer of sig's key and algorithm. A key that doesn't
// fit the algorithm, or an HMAC algorithm, is ErrUnsupportedAlgorithm; a
// key of another type is ErrUnsupportedKeyType. opts may be nullptr.
std::pair<ptr<Signer>, error> NewSigner(const SigningKey& sig, const SignerOptions* opts);

}  // namespace gopp::jose
