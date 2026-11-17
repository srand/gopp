// JSON Web Signatures, ported from go-jose v4.1.7's jws.go, the header
// handling of shared.go, and the verification of signing.go.
#include "internal.hpp"

#include <gopp/encoding/base64.hpp>
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <vector>

namespace gopp::jose {

const error ErrCryptoFailure = errors::New("go-jose/go-jose: error in cryptographic primitive");
const error ErrUnsupportedAlgorithm = errors::New("go-jose/go-jose: unknown/unsupported algorithm");
const error ErrNotSupported = errors::New("go-jose/go-jose: compact serialization not supported for object");
const error ErrUnprotectedNonce = errors::New("go-jose/go-jose: Nonce parameter included in unprotected header");
const error ErrUnsupportedCriticalHeader = errors::New("go-jose/go-jose: unsupported critical header");
const error ErrNotPublic = errors::New("go-jose/go-jose: public key was unexpectedly not public");
const error ErrJWKSKidNotFound = errors::New("go-jose/go-jose: JWK with matching kid not found in JWK Set");

string ErrUnexpectedSignatureAlgorithm::Error() const {
    // Go's %q of the algorithm and of the list of them.
    std::string s = "unexpected signature algorithm " + std::string(strconv::Quote(Got)) + "; expected [";
    for (size_t i = 0; i < expected.size(); i++) {
        if (i > 0) s += ' ';
        s += strconv::Quote(expected[i]);
    }
    return string(s + "]");
}

namespace internal {

const error errEmptyInput = errors::New("go-jose/go-jose: empty input");
const error errInvalidVerificationKey = errors::New("go-jose/go-jose: invalid verification key");

// ---------------------------------------------------------------------------
// rawHeader

namespace {

// The member k, if it is there and not null.
const json::RawMessage* member(const rawHeader& h, std::string_view k) {
    auto it = h.find(k);
    if (it == h.end() || !it->second) return nullptr;
    return &*it->second;
}

std::pair<std::vector<string>, error> getCritical(const rawHeader& h) {
    const auto* v = member(h, "crit");
    if (!v) return {{}, nullptr};
    std::vector<string> q;
    if (error err = json::UnmarshalCaseSensitive(v->data, &q)) return {{}, err};
    return {q, nullptr};
}

// Go's fmt.Errorf("failed to unmarshal <what>: %v: %#v", err, string(*v)).
error unmarshalError(std::string_view what, const error& err, const json::RawMessage& v) {
    return errors::New("failed to unmarshal " + string(what) + ": " + err.Error() + ": " +
                       strconv::Quote(asStringView(v.data)));
}

}  // namespace

string getString(const rawHeader& h, std::string_view k) {
    const auto* v = member(h, k);
    if (!v) return "";
    string s;
    if (json::UnmarshalCaseSensitive(v->data, &s)) return "";
    return s;
}

error checkNoCritical(const rawHeader& h) {
    if (h.contains("crit")) return ErrUnsupportedCriticalHeader;
    return nullptr;
}

error checkSupportedCritical(const rawHeader& h) {
    auto [crit, err] = getCritical(h);
    if (err) return err;
    for (const auto& name : crit) {
        if (name != "b64") return ErrUnsupportedCriticalHeader;
    }
    return nullptr;
}

std::pair<bool, error> getB64(const rawHeader& h) {
    const auto* v = member(h, "b64");
    if (!v) return {true, nullptr};
    bool b64 = false;
    if (error err = json::UnmarshalCaseSensitive(v->data, &b64)) return {true, err};
    return {b64, nullptr};
}

std::pair<Header, error> sanitized(const rawHeader& parsed) {
    Header h;
    for (const auto& [k, v] : parsed) {
        if (!v) continue;
        if (k == "jwk") {
            auto jwk = make_ptr<JSONWebKey>();
            if (error err = unmarshalPublicJWK(*jwk, v->data)) {
                return {Header{}, fmt::Errorf("failed to unmarshal JWK: {}", fmt::W(err))};
            }
            h.JSONWebKey = jwk;
        } else if (k == "kid" || k == "alg" || k == "nonce") {
            string s;
            if (error err = json::UnmarshalCaseSensitive(v->data, &s)) {
                return {Header{}, unmarshalError(k == "kid" ? "key ID" : k == "alg" ? "algorithm" : "nonce", err, *v)};
            }
            (k == "kid" ? h.KeyID : k == "alg" ? h.Algorithm : h.Nonce) = s;
        } else if (k == "x5c") {
            // go++ doesn't parse certificates: a list of strings is
            // accepted, and dropped.
            std::vector<string> c;
            if (error err = json::UnmarshalCaseSensitive(v->data, &c)) return {Header{}, unmarshalError("x5c header", err, *v)};
        } else {
            json::Value v2;
            if (error err = json::UnmarshalCaseSensitive(v->data, &v2)) return {Header{}, unmarshalError("value", err, *v)};
            h.ExtraHeaders[k] = std::move(v2);
        }
    }
    return {h, nullptr};
}

void merge(rawHeader& dst, const std::optional<rawHeader>& src) {
    if (!src) return;
    for (const auto& [k, v] : *src) {
        if (!dst.contains(k)) dst.emplace(k, v);
    }
}

std::pair<std::optional<rawHeader>, error> parseProtectedHeader(const std::optional<byteBuffer>& in) {
    if (!in || in->bytes().empty()) return {std::nullopt, nullptr};
    std::optional<rawHeader> header;
    if (error err = json::UnmarshalCaseSensitive(in->bytes(), &header); err || !header) {
        return {std::nullopt, errors::New("go-jose/go-jose: invalid protected header")};
    }
    return {header, nullptr};
}

// ---------------------------------------------------------------------------
// rawJSONWebSignature

rawHeader mergedHeaders(const Signature& sig) {
    rawHeader out;
    merge(out, sig.protected_);
    merge(out, sig.header_);
    return out;
}

std::pair<slice<byte>, error> computeAuthData(std::span<const byte> payload, const Signature& sig) {
    std::string authData;
    rawHeader protectedHeader;

    if (sig.originalProtected_) {
        if (error err = json::UnmarshalCaseSensitive(*sig.originalProtected_, &protectedHeader)) return {slice<byte>(), err};
        authData += base64::RawURLEncoding.EncodeToString(*sig.originalProtected_);
    } else if (sig.protected_) {
        protectedHeader = *sig.protected_;
        auto [serializedProtected, err] = json::Marshal(protectedHeader);
        if (err) return {slice<byte>(), err};
        authData += base64::RawURLEncoding.EncodeToString(serializedProtected);
    }

    auto [needsBase64, err] = getB64(protectedHeader);
    if (err) needsBase64 = true;

    authData += '.';
    if (needsBase64) authData += base64::RawURLEncoding.EncodeToString(payload);
    else authData += asStringView(payload);
    return {toBytes(authData), nullptr};
}

std::pair<ptr<JSONWebSignature>, error> rawJSONWebSignature::sanitized(std::span<const std::string_view> algs) const {
    if (algs.empty()) return {nullptr, errors::New("go-jose/go-jose: no signature algorithms specified")};
    if (!Payload) return {nullptr, errors::New("go-jose/go-jose: missing payload in JWS message")};

    auto obj = make_ptr<JSONWebSignature>();
    obj->payload = Payload->data;

    // No signatures array: the flattened serialization, of which the
    // compact one is a part. There is no unprotected header.
    jose::Signature signature;
    auto [prot, err] = parseProtectedHeader(Protected);
    if (err) return {nullptr, err};
    signature.protected_ = std::move(prot);
    if (Signature) signature.signature = Signature->data;
    // The protected header as it was, to sign: it may hold members that
    // marshaling the parsed one would change.
    if (Protected) signature.originalProtected_ = Protected->data;

    rawHeader merged = mergedHeaders(signature);
    string alg = getSignatureAlgorithm(merged);
    if (std::find(algs.begin(), algs.end(), std::string_view(alg)) == algs.end()) {
        auto e = make_ptr<ErrUnexpectedSignatureAlgorithm>();
        e->Got = alg;
        for (auto a : algs) e->expected = append(e->expected, SignatureAlgorithm(a));
        return {nullptr, e};
    }

    std::tie(signature.Header, err) = internal::sanitized(merged);
    if (err) return {nullptr, err};
    if (signature.header_) {
        std::tie(signature.Unprotected, err) = internal::sanitized(*signature.header_);
        if (err) return {nullptr, err};
    }
    if (signature.protected_) {
        std::tie(signature.Protected, err) = internal::sanitized(*signature.protected_);
        if (err) return {nullptr, err};
    }

    // As per RFC 7515 Section 4.1.3, only public keys are allowed to be embedded.
    const auto& jwk = signature.Header.JSONWebKey;
    if (jwk && (!jwk->Valid() || !jwk->IsPublic())) {
        return {nullptr, errors::New("go-jose/go-jose: invalid embedded jwk, must be public key")};
    }

    obj->Signatures = append(obj->Signatures, std::move(signature));
    return {obj, nullptr};
}

// ---------------------------------------------------------------------------
// Verification (signing.go)

std::pair<const object*, error> tryJWKS(const object& key, const Header& header) {
    const auto* jwks = dynamic_cast<const JSONWebKeySet*>(&key);
    // If the specified key is not a JWKS, return as is.
    if (!jwks) return {&key, nullptr};

    // If no KID is specified in the header, reject.
    if (header.KeyID.empty()) return {nullptr, ErrJWKSKidNotFound};
    // The first JWK with the matching KID.
    for (const auto& k : jwks->Keys) {
        if (k.KeyID == header.KeyID) return {k.Key.get(), nullptr};
    }
    return {nullptr, ErrJWKSKidNotFound};
}

std::pair<std::unique_ptr<payloadVerifier>, error> newVerifier(const object* verificationKey) {
    if (const auto* k = dynamic_cast<const ed25519::PublicKey*>(verificationKey)) {
        // Go checks for an empty key, and its ed25519.Verify panics on any
        // other size than 32 bytes.
        if (k->key.size() != static_cast<size_t>(ed25519::PublicKeySize)) return {nullptr, errInvalidVerificationKey};
        auto v = std::make_unique<edEncrypterVerifier>();
        v->publicKey = k;
        return {std::move(v), nullptr};
    }
    if (const auto* k = dynamic_cast<const rsa::PublicKey*>(verificationKey)) {
        if (k->N.Sign() <= 0) return {nullptr, errInvalidVerificationKey};  // Go's nil
        auto v = std::make_unique<rsaEncrypterVerifier>();
        v->publicKey = k;
        return {std::move(v), nullptr};
    }
    if (const auto* k = dynamic_cast<const ecdsa::PublicKey*>(verificationKey)) {
        if (!k->Curve) return {nullptr, errInvalidVerificationKey};  // Go's nil
        auto v = std::make_unique<ecEncrypterVerifier>();
        v->publicKey = k;
        return {std::move(v), nullptr};
    }
    if (const auto* k = dynamic_cast<const JSONWebKey*>(verificationKey)) return newVerifier(k->Key.get());
    return {nullptr, ErrUnsupportedKeyType};
}

namespace {

// obj.DetachedVerify(payload, verificationKey)
error detachedVerify(const JSONWebSignature& obj, std::span<const byte> payload, const object& verificationKey) {
    if (obj.Signatures.size() > 1) {
        return errors::New("go-jose/go-jose: too many signatures in payload; expecting only one");
    }
    // Go indexes the first signature, and panics without one.
    if (obj.Signatures.empty()) return errors::New("go-jose/go-jose: no signature in payload");
    const Signature& signature = obj.Signatures[0];

    auto [key, err] = tryJWKS(verificationKey, signature.Header);
    if (err) return err;
    auto [verifier, err2] = newVerifier(key);
    if (err2) return err2;

    if (signature.header_) {
        // Per https://www.rfc-editor.org/rfc/rfc7515.html#section-4.1.11,
        // "crit" MUST occur only within the JWS Protected Header.
        if (error e = checkNoCritical(*signature.header_)) return e;
    }
    if (signature.protected_) {
        if (error e = checkSupportedCritical(*signature.protected_)) return e;
    }

    auto [input, err3] = computeAuthData(payload, signature);
    if (err3) return ErrCryptoFailure;

    string alg = getSignatureAlgorithm(mergedHeaders(signature));
    if (verifier->verifyPayload(input, signature.signature, alg)) return ErrCryptoFailure;
    return nullptr;
}

const char* const errThreeParts = "go-jose/go-jose: compact JWS format must have three parts";

// parseSignedCompact: a compact message (without a detached payload).
std::pair<ptr<JSONWebSignature>, error> parseSignedCompact(std::string_view input,
                                                           std::span<const std::string_view> algs) {
    if (input.empty()) return {nullptr, errEmptyInput};

    auto dot1 = input.find('.');
    if (dot1 == std::string_view::npos) return {nullptr, errors::New(errThreeParts)};  // no period found
    std::string_view protectedPart = input.substr(0, dot1), rest = input.substr(dot1 + 1);
    auto dot2 = rest.find('.');
    if (dot2 == std::string_view::npos) return {nullptr, errors::New(errThreeParts)};  // only one period found
    std::string_view claims = rest.substr(0, dot2), sig = rest.substr(dot2 + 1);
    if (sig.find('.') != std::string_view::npos) return {nullptr, errors::New(errThreeParts)};  // too many

    auto [rawProtected, err] = base64::RawURLEncoding.DecodeString(protectedPart);
    if (err) return {nullptr, err};
    auto [payload, err2] = base64::RawURLEncoding.DecodeString(claims);
    if (err2) return {nullptr, err2};
    auto [signature, err3] = base64::RawURLEncoding.DecodeString(sig);
    if (err3) return {nullptr, err3};

    rawJSONWebSignature raw;
    raw.Payload = byteBuffer{payload};
    raw.Protected = byteBuffer{rawProtected};
    raw.Signature = byteBuffer{signature};
    return raw.sanitized(algs);
}

}  // namespace

}  // namespace internal

// ---------------------------------------------------------------------------
// JSONWebSignature

std::pair<slice<byte>, error> JSONWebSignature::Verify(const object& verificationKey) const {
    if (error err = internal::detachedVerify(*this, payload, verificationKey)) return {slice<byte>(), err};
    return {payload, nullptr};
}

std::pair<slice<byte>, error> JSONWebSignature::Verify(const JSONWebKey& k) const {
    return Verify(static_cast<const object&>(k));
}

slice<byte> JSONWebSignature::UnsafePayloadWithoutVerification() const { return payload; }

std::pair<string, error> JSONWebSignature::CompactSerialize() const {
    if (Signatures.size() != 1 || Signatures[0].header_ || !Signatures[0].protected_) return {"", ErrNotSupported};
    auto [serializedProtected, err] = json::Marshal(*Signatures[0].protected_);
    if (err) return {"", err};
    const auto& enc = base64::RawURLEncoding;
    return {enc.EncodeToString(serializedProtected) + "." + enc.EncodeToString(payload) + "." +
                enc.EncodeToString(Signatures[0].signature),
            nullptr};
}

std::pair<ptr<JSONWebSignature>, error> ParseSigned(std::string_view s,
                                                    std::span<const std::string_view> allowedAlgs) {
    std::string_view trimmed = s;
    while (!trimmed.empty()) {
        auto [r, size] = utf8::DecodeRuneInString(trimmed);
        if (!unicode::IsSpace(r)) break;
        trimmed.remove_prefix(static_cast<size_t>(size));
    }
    // The JSON serializations are not supported.
    if (trimmed.starts_with('{')) return {nullptr, ErrNotSupported};
    return internal::parseSignedCompact(internal::stripWhitespace(s), allowedAlgs);
}

std::pair<ptr<JSONWebSignature>, error> ParseSignedCompact(std::string_view s,
                                                           std::span<const std::string_view> allowedAlgs) {
    return internal::parseSignedCompact(s, allowedAlgs);
}

}  // namespace gopp::jose
