// Signing, ported from go-jose v4.1.7's signing.go, and the RSA, ECDSA and
// Ed25519 signers and verifiers of asymmetric.go.
#include "internal.hpp"

#include <gopp/encoding/base64.hpp>

namespace gopp::jose {

SignerOptions& SignerOptions::WithType(std::string_view t) { return WithHeader("typ", json::Value(t)); }

SignerOptions& SignerOptions::WithHeader(std::string_view k, json::Value v) {
    ExtraHeaders[string(k)] = std::move(v);
    return *this;
}

namespace internal {

namespace {

// The hash of the RSA algorithms; zero for the others.
crypto::Hash rsaHash(std::string_view alg) {
    if (alg == RS256 || alg == PS256) return crypto::Hash::SHA256;
    if (alg == RS384 || alg == PS384) return crypto::Hash::SHA384;
    if (alg == RS512 || alg == PS512) return crypto::Hash::SHA512;
    return crypto::Hash{};
}

bool isPSS(std::string_view alg) { return alg == PS256 || alg == PS384 || alg == PS512; }

slice<byte> digest(crypto::Hash hash, std::span<const byte> payload) {
    auto hasher = crypto::New(hash);
    hasher->Write(payload);
    return hasher->Sum(slice<byte>());
}

Signature signatureOf(slice<byte> sig) {
    Signature out;
    out.signature = std::move(sig);
    out.protected_ = rawHeader{};
    return out;
}

ptr<JSONWebKey> publicJWK(ptr<object> pub) {
    auto jwk = make_ptr<JSONWebKey>();
    jwk->Key = std::move(pub);
    return jwk;
}

}  // namespace

// ---------------------------------------------------------------------------
// RSA

std::pair<recipientSigInfo, error> newRSASigner(std::string_view alg, ptr<rsa::PrivateKey> privateKey) {
    if (rsaHash(alg) == crypto::Hash{}) return {{}, ErrUnsupportedAlgorithm};
    if (!privateKey) return {{}, errors::New("invalid private key")};
    recipientSigInfo r;
    r.sigAlg = alg;
    r.publicKey = publicJWK(privateKey->Public());
    auto signer = make_ptr<rsaDecrypterSigner>();
    signer->privateKey = std::move(privateKey);
    r.signer = signer;
    return {r, nullptr};
}

std::pair<Signature, error> rsaDecrypterSigner::signPayload(std::span<const byte> payload, std::string_view alg) const {
    crypto::Hash hash = rsaHash(alg);
    if (hash == crypto::Hash{}) return {Signature{}, ErrUnsupportedAlgorithm};
    auto hashed = digest(hash, payload);

    std::pair<slice<byte>, error> out;
    if (isPSS(alg)) {
        rsa::PSSOptions opts;
        opts.SaltLength = rsa::PSSSaltLengthEqualsHash;
        out = rsa::SignPSS(nullptr, *privateKey, hash, hashed, &opts);
    } else {
        out = rsa::SignPKCS1v15(nullptr, *privateKey, hash, hashed);
    }
    if (out.second) return {Signature{}, out.second};
    return {signatureOf(std::move(out.first)), nullptr};
}

error rsaEncrypterVerifier::verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                                          std::string_view alg) const {
    crypto::Hash hash = rsaHash(alg);
    if (hash == crypto::Hash{}) return ErrUnsupportedAlgorithm;
    if (!publicKey) return errInvalidVerificationKey;
    auto hashed = digest(hash, payload);
    if (isPSS(alg)) return rsa::VerifyPSS(*publicKey, hash, hashed, signature, nullptr);
    return rsa::VerifyPKCS1v15(*publicKey, hash, hashed, signature);
}

// ---------------------------------------------------------------------------
// ECDSA

namespace {

// The hash and curve of an ECDSA algorithm; a null curve for the others.
struct ecParams {
    crypto::Hash hash{};
    ptr<elliptic::Curve> curve;
    int keySize = 0;  // the size of r and s in bytes
};

ecParams ecParamsOf(std::string_view alg) {
    if (alg == ES256) return {crypto::Hash::SHA256, elliptic::P256(), 32};
    if (alg == ES384) return {crypto::Hash::SHA384, elliptic::P384(), 48};
    if (alg == ES512) return {crypto::Hash::SHA512, elliptic::P521(), 66};
    return {};
}

}  // namespace

std::pair<recipientSigInfo, error> newECDSASigner(std::string_view alg, ptr<ecdsa::PrivateKey> privateKey) {
    if (!ecParamsOf(alg).curve) return {{}, ErrUnsupportedAlgorithm};
    if (!privateKey) return {{}, errors::New("invalid private key")};
    recipientSigInfo r;
    r.sigAlg = alg;
    r.publicKey = publicJWK(privateKey->Public());
    auto signer = make_ptr<ecDecrypterSigner>();
    signer->privateKey = std::move(privateKey);
    r.signer = signer;
    return {r, nullptr};
}

std::pair<Signature, error> ecDecrypterSigner::signPayload(std::span<const byte> payload, std::string_view alg) const {
    // Go's goes on with a zero bit size and hash for another algorithm, and
    // fails on the size.
    ecParams p = ecParamsOf(alg);
    if (!p.curve) return {Signature{}, ErrUnsupportedAlgorithm};
    const auto& curve = privateKey->PublicKey.Curve;
    int expectedBitSize = p.curve->Params().BitSize;
    int curveBits = curve ? curve->Params().BitSize : 0;
    if (expectedBitSize != curveBits) {
        return {Signature{}, errors::New("go-jose/go-jose: expected " + std::to_string(expectedBitSize) +
                                         " bit key, got " + std::to_string(curveBits) + " bits instead")};
    }

    auto [r, s, err] = ecdsa::Sign(nullptr, *privateKey, digest(p.hash, payload));
    if (err) return {Signature{}, err};

    int keyBytes = curveBits / 8 + (curveBits % 8 > 0 ? 1 : 0);
    auto out = slice<byte>::make(2 * keyBytes);
    r.FillBytes(out.span().subspan(0, keyBytes));
    s.FillBytes(out.span().subspan(keyBytes));
    return {signatureOf(std::move(out)), nullptr};
}

error ecEncrypterVerifier::verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                                         std::string_view alg) const {
    ecParams p = ecParamsOf(alg);
    if (!p.curve) return ErrUnsupportedAlgorithm;
    if (!publicKey || publicKey->Curve.get() != p.curve.get()) {
        return errors::New("go-jose/go-jose: signature uses different algorithm than public key");
    }
    if (signature.size() != static_cast<size_t>(2 * p.keySize)) {
        return errors::New("go-jose/go-jose: invalid signature size, have " + std::to_string(signature.size()) +
                           " bytes, wanted " + std::to_string(2 * p.keySize));
    }

    big::Int r, s;
    r.SetBytes(signature.subspan(0, p.keySize));
    s.SetBytes(signature.subspan(p.keySize));
    if (!ecdsa::Verify(*publicKey, digest(p.hash, payload), r, s)) {
        return errors::New("go-jose/go-jose: ecdsa signature failed to verify");
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Ed25519

std::pair<recipientSigInfo, error> newEd25519Signer(std::string_view alg, ptr<ed25519::PrivateKey> privateKey) {
    if (alg != EdDSA) return {{}, ErrUnsupportedAlgorithm};
    if (!privateKey) return {{}, errors::New("invalid private key")};
    recipientSigInfo r;
    r.sigAlg = alg;
    r.publicKey = publicJWK(privateKey->Public());
    auto signer = make_ptr<edDecrypterSigner>();
    signer->privateKey = std::move(privateKey);
    r.signer = signer;
    return {r, nullptr};
}

std::pair<Signature, error> edDecrypterSigner::signPayload(std::span<const byte> payload, std::string_view alg) const {
    if (alg != EdDSA) return {Signature{}, ErrUnsupportedAlgorithm};
    // Go's panics on a key of another size.
    if (privateKey->key.size() != static_cast<size_t>(ed25519::PrivateKeySize)) {
        return {Signature{}, errors::New("go-jose/go-jose: invalid Ed25519 private key length")};
    }
    return {signatureOf(ed25519::Sign(*privateKey, payload)), nullptr};
}

error edEncrypterVerifier::verifyPayload(std::span<const byte> payload, std::span<const byte> signature,
                                         std::string_view alg) const {
    if (alg != EdDSA) return ErrUnsupportedAlgorithm;
    // ed25519::Verify panics on a key of another size, as Go's does.
    if (!publicKey || publicKey->key.size() != static_cast<size_t>(ed25519::PublicKeySize)) {
        return errInvalidVerificationKey;
    }
    if (!ed25519::Verify(*publicKey, payload, signature)) {
        return errors::New("go-jose/go-jose: ed25519 signature failed to verify");
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Signers

namespace {

std::pair<recipientSigInfo, error> newJWKSigner(std::string_view alg, const JSONWebKey& signingKey) {
    auto [recipient, err] = makeJWSRecipient(alg, signingKey.Key);
    if (err) return {{}, err};
    if (recipient.publicKey) {
        // This should be impossible, but let's check anyway.
        if (!recipient.publicKey->IsPublic()) return {{}, ErrNotPublic};
        // The public key of the inner key, with the key ID and the other
        // parameters of signingKey.
        auto publicKey = make_ptr<JSONWebKey>(signingKey);
        publicKey->Key = recipient.publicKey->Key;
        recipient.publicKey = publicKey;
    }
    return {recipient, nullptr};
}

// genericSigner, of one recipient.
struct genericSigner final : Signer {
    recipientSigInfo recipient;
    bool embedJWK = false;
    std::map<string, json::Value> extraHeaders;

    std::pair<ptr<JSONWebSignature>, error> Sign(std::span<const byte> payload) override {
        auto obj = make_ptr<JSONWebSignature>();
        obj->payload = slice<byte>::of(payload);

        rawHeader protectedHeader;
        auto set = [&](std::string_view k, const auto& v) -> error {
            if (error err = setHeader(protectedHeader, k, v)) {
                return errors::New("go-jose/go-jose: Error marshaling JSON: " + err.Error());
            }
            return nullptr;
        };
        if (error err = set("alg", recipient.sigAlg)) return {nullptr, err};

        if (recipient.publicKey) {
            // We want to embed the JWK or set the kid header, but not both.
            // See https://github.com/square/go-jose/issues/157.
            if (embedJWK) {
                if (error err = set("jwk", *recipient.publicKey)) return {nullptr, err};
            } else if (!recipient.publicKey->KeyID.empty()) {
                if (error err = set("kid", recipient.publicKey->KeyID)) return {nullptr, err};
            }
        }

        bool needsBase64 = true;
        for (const auto& [k, v] : extraHeaders) {
            if (error err = set(k, v)) return {nullptr, err};
            if (k == "b64") {
                if (!v.IsBool()) return {nullptr, errors::New("go-jose/go-jose: Invalid b64 header parameter")};
                needsBase64 = v.AsBool();
            }
        }

        auto [serializedProtected, err] = json::Marshal(protectedHeader);
        if (err) return {nullptr, errors::New("go-jose/go-jose: Error marshaling JSON: " + err.Error())};

        std::string input(base64::RawURLEncoding.EncodeToString(serializedProtected));
        input += '.';
        if (needsBase64) input += base64::RawURLEncoding.EncodeToString(payload);
        else input += asStringView(payload);

        auto [signatureInfo, err2] = recipient.signer->signPayload(as_bytes(input), recipient.sigAlg);
        if (err2) return {nullptr, err2};
        signatureInfo.protected_ = std::move(protectedHeader);
        obj->Signatures = append(obj->Signatures, std::move(signatureInfo));
        return {obj, nullptr};
    }

    SignerOptions Options() const override {
        SignerOptions opts;
        opts.ExtraHeaders = extraHeaders;
        opts.EmbedJWK = embedJWK;
        return opts;
    }
};

}  // namespace

std::pair<recipientSigInfo, error> makeJWSRecipient(std::string_view alg, const ptr<object>& signingKey) {
    if (auto k = dynamic_pointer_cast<ed25519::PrivateKey>(signingKey)) return newEd25519Signer(alg, k);
    if (auto k = dynamic_pointer_cast<rsa::PrivateKey>(signingKey)) return newRSASigner(alg, k);
    if (auto k = dynamic_pointer_cast<ecdsa::PrivateKey>(signingKey)) return newECDSASigner(alg, k);
    if (auto k = dynamic_pointer_cast<JSONWebKey>(signingKey)) return newJWKSigner(alg, *k);
    return {{}, ErrUnsupportedKeyType};
}

}  // namespace internal

std::pair<ptr<Signer>, error> NewSigner(const SigningKey& sig, const SignerOptions* opts) {
    auto signer = make_ptr<internal::genericSigner>();
    if (opts) {
        signer->embedJWK = opts->EmbedJWK;
        signer->extraHeaders = opts->ExtraHeaders;
    }
    auto [recipient, err] = internal::makeJWSRecipient(sig.Algorithm, sig.Key);
    if (err) return {nullptr, err};
    signer->recipient = std::move(recipient);
    return {signer, nullptr};
}

}  // namespace gopp::jose
