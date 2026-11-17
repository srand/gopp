// x/jose's internals: go-jose's encoding.go (the base64url byteBuffer and
// the helpers that make one), jwk.go's rawJSONWebKey with its conversions
// to and from keys, shared.go's rawHeader, jws.go's rawJSONWebSignature, and
// the signers and verifiers of signing.go and asymmetric.go.
#pragma once

#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/jose.hpp>
#include <gopp/math/big.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace gopp::jose::internal {

// stripWhitespace: data without its white space (unicode::IsSpace). As
// Go's, which ranges over the runes of data, it turns each byte that isn't
// valid UTF-8 into U+FFFD.
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

// unmarshalPublicJWK: go-jose's k.unmarshalJSON(data, false): a JWK that
// must not hold private (or symmetric) parameters, as one embedded in a
// header.
error unmarshalPublicJWK(JSONWebKey& k, std::span<const byte> data);

// ---------------------------------------------------------------------------
// shared.go

// errEmptyInput: "go-jose/go-jose: empty input".
extern const error errEmptyInput;
// errInvalidVerificationKey: "go-jose/go-jose: invalid verification key";
// Go's for a nil key, go++'s for an empty one.
extern const error errInvalidVerificationKey;

using rawHeader = detail::rawHeader;

// h.set(k, v): sets the member k to the JSON of v.
template <typename T>
error setHeader(rawHeader& h, std::string_view k, const T& v) {
    auto [b, err] = json::Marshal(v);
    if (err) return err;
    h[string(k)] = json::RawMessage{b};
    return nullptr;
}

// h.getString(k): the string member k; "" if it is missing or not a string.
string getString(const rawHeader& h, std::string_view k);
inline string getSignatureAlgorithm(const rawHeader& h) { return getString(h, "alg"); }
inline string getNonce(const rawHeader& h) { return getString(h, "nonce"); }
// h.checkNoCritical(): ErrUnsupportedCriticalHeader if h has "crit".
error checkNoCritical(const rawHeader& h);
// h.checkSupportedCritical(): an error unless each name of "crit" is "b64".
error checkSupportedCritical(const rawHeader& h);
// h.getB64(): the "b64" member; true if missing.
std::pair<bool, error> getB64(const rawHeader& h);
// h.sanitized(): the Header of h.
std::pair<Header, error> sanitized(const rawHeader& h);
// dst.merge(src): adds the members of src that dst doesn't have.
void merge(rawHeader& dst, const std::optional<rawHeader>& src);
// parseProtectedHeader: the header of in; none if in is missing or empty.
std::pair<std::optional<rawHeader>, error> parseProtectedHeader(const std::optional<byteBuffer>& in);

// ---------------------------------------------------------------------------
// jws.go

// rawJSONWebSignature: a parsed message, before it is checked. Only the
// members of a compact one.
struct rawJSONWebSignature {
    std::optional<byteBuffer> Payload;
    std::optional<byteBuffer> Protected;
    std::optional<byteBuffer> Signature;

    // parsed.sanitized(algs): the JSONWebSignature, if its algorithm is one
    // of algs.
    std::pair<ptr<JSONWebSignature>, error> sanitized(std::span<const std::string_view> algs) const;
};

// sig.mergedHeaders(): the protected header, and the unprotected members
// it doesn't have.
rawHeader mergedHeaders(const Signature& sig);
// obj.computeAuthData(payload, sig): the signing input.
std::pair<slice<byte>, error> computeAuthData(std::span<const byte> payload, const Signature& sig);

// ---------------------------------------------------------------------------
// signing.go and asymmetric.go

// payloadSigner: signs the signing input with alg; the Signature has the
// signature and an empty protected header.
struct payloadSigner : virtual object {
    virtual std::pair<Signature, error> signPayload(std::span<const byte> payload, std::string_view alg) const = 0;
};

// payloadVerifier: nil if signature is alg's signature of payload.
struct payloadVerifier {
    virtual ~payloadVerifier() = default;
    virtual error verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                                std::string_view alg) const = 0;
};

// The verifiers point to keys they don't own: the key of the Verify call.
struct rsaEncrypterVerifier final : payloadVerifier {
    const rsa::PublicKey* publicKey = nullptr;
    error verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                        std::string_view alg) const override;
};

struct ecEncrypterVerifier final : payloadVerifier {
    const ecdsa::PublicKey* publicKey = nullptr;
    error verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                        std::string_view alg) const override;
};

struct edEncrypterVerifier final : payloadVerifier {
    const ed25519::PublicKey* publicKey = nullptr;
    error verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                        std::string_view alg) const override;
};

struct rsaDecrypterSigner final : payloadSigner {
    ptr<rsa::PrivateKey> privateKey;
    std::pair<Signature, error> signPayload(std::span<const byte> payload, std::string_view alg) const override;
};

struct ecDecrypterSigner final : payloadSigner {
    ptr<ecdsa::PrivateKey> privateKey;
    std::pair<Signature, error> signPayload(std::span<const byte> payload, std::string_view alg) const override;
};

struct edDecrypterSigner final : payloadSigner {
    ptr<ed25519::PrivateKey> privateKey;
    std::pair<Signature, error> signPayload(std::span<const byte> payload, std::string_view alg) const override;
};

// recipientSigInfo: a signer, its algorithm, and the public key to embed
// or name.
struct recipientSigInfo {
    string sigAlg;
    ptr<JSONWebKey> publicKey;
    ptr<payloadSigner> signer;
};

std::pair<recipientSigInfo, error> newRSASigner(std::string_view alg, ptr<rsa::PrivateKey> privateKey);
std::pair<recipientSigInfo, error> newECDSASigner(std::string_view alg, ptr<ecdsa::PrivateKey> privateKey);
std::pair<recipientSigInfo, error> newEd25519Signer(std::string_view alg, ptr<ed25519::PrivateKey> privateKey);
// makeJWSRecipient: the recipientSigInfo of a private key, or of a JWK
// holding one.
std::pair<recipientSigInfo, error> makeJWSRecipient(std::string_view alg, const ptr<object>& signingKey);

// newVerifier: the verifier of a public key, or of a JSONWebKey holding
// one. ErrUnsupportedKeyType for other keys.
std::pair<std::unique_ptr<payloadVerifier>, error> newVerifier(const object* verificationKey);
// tryJWKS: the first key of header's "kid" if key is a JSONWebKeySet, and
// key otherwise.
std::pair<const object*, error> tryJWKS(const object& key, const Header& header);

}  // namespace gopp::jose::internal
