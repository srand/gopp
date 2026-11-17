// Ported from github.com/go-jose/go-jose/v4's jws_test.go, signing_test.go
// and asymmetric_test.go (v4.1.7; Apache 2.0, see x/jose/LICENSE), with the
// names of their tests: TestParseEmptySigned, TestCompactParseJWS,
// TestRejectUnprotectedJWSNonce, TestHeaderFieldsCompact,
// TestErrorMissingPayloadJWS, TestErrorUnexpectedSignatureAlgorithmInProtected,
// TestUnexpectedAlgorithmCheckedBeforeHeaders, TestNullHeaderValue,
// TestRejectNullProtectedHeader, TestEmbeddedMalformedEd25519JWK,
// TestEmbeddedPrivateJWKRejected, TestSampleNimbusJWSMessagesRSA and EC;
// TestRoundtripsJWS, TestRoundtripsJWSCorruptSignature, TestJWSInvalidKey,
// TestJWSNilVerificationKey, TestInvalidSignerAlg, TestInvalidJWS,
// TestSignerKid, TestEmbedJwk, TestSignerOptionsEd, TestSignerOptions,
// TestSignerExtraHeaderInclusion; TestEd25519, TestInvalidAlgorithmsRSA,
// TestInvalidAlgorithmsEC, TestInvalidECPublicKey, TestInvalidAlgorithmEC.
//
// Only the compact serialization is parsed: the messages of the JSON
// serializations are ErrNotSupported here, and the tests that use them are
// ported to compact messages, or check that. Left out: the JSON
// serialization tests (TestFullParseJWS, TestHeaderFieldsFull,
// TestParseSignedJSONDoesNotRewriteMemberNames,
// TestVerifyFlattenedWith*, TestErrorUnexpectedSignatureAlgorithmInSignatures,
// TestGeneralSerializationUnprotectedHeader, TestParseSignedMaxSignatures,
// TestMultiRecipientJWS, TestVerifyMultiJWKS*), the detached payload and b64
// tests (TestDetachedVerifyJWS, TestDetachedCompactSerialization,
// TestJWSComputeAuthDataBase64, TestSignerB64), the HMAC tests
// (TestEmbeddedHMAC, TestInvalidHMACKeySize; go++ has no symmetric keys, and
// the HMAC algorithms of the round trips are ErrUnsupportedAlgorithm), the
// X.509 tests (TestJWSWithCertificateChain, TestEmbedJWKBug), the
// encryption parts of the asymmetric tests, NonceSource (the nonce of the
// round trips is an extra header) and TestSignerWithBrokenRand (the random
// source is OpenSSL's). Go's (*rsa.PublicKey)(nil) has no counterpart; an
// empty key stands for it.
//
// The tests after TestInvalidAlgorithmEC's (RejectsAlgNone and on) pin what
// verification does with a message an attacker made: the algorithms that
// never verify, and keys that don't fit the algorithm.
#include <gopp/crypto.hpp>
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/hmac.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/jose.hpp>

#include <gtest/gtest.h>

#include "../x/jose/src/internal.hpp"

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace gopp;
namespace jose = gopp::jose;
using namespace gopp::jose::internal;

namespace {

struct TestKeys {
    ptr<rsa::PrivateKey> rsa;
    ptr<ecdsa::PrivateKey> ec256, ec384, ec521;
    ptr<ed25519::PublicKey> edPub;
    ptr<ed25519::PrivateKey> edPriv;
};

const TestKeys& keys() {
    static const TestKeys k = [] {
        TestKeys k;
        auto [r, err] = rsa::GenerateKey(nullptr, 2048);
        EXPECT_FALSE(err);
        k.rsa = r;
        k.ec256 = ecdsa::GenerateKey(elliptic::P256(), nullptr).first;
        k.ec384 = ecdsa::GenerateKey(elliptic::P384(), nullptr).first;
        k.ec521 = ecdsa::GenerateKey(elliptic::P521(), nullptr).first;
        auto [pub, priv, err2] = ed25519::GenerateKey(nullptr);
        EXPECT_FALSE(err2);
        k.edPub = pub;
        k.edPriv = priv;
        return k;
    }();
    return k;
}

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

std::string b64(std::string_view s) { return std::string(base64::RawURLEncoding.EncodeToString(as_bytes(s))); }
std::string b64(std::span<const byte> b) { return std::string(base64::RawURLEncoding.EncodeToString(b)); }

bool contains(const error& err, std::string_view sub) {
    return err && std::string_view(err.Error()).find(sub) != std::string_view::npos;
}

slice<byte> fromHex(std::string_view h) {
    auto [b, err] = hex::DecodeString(stripWhitespace(h));
    EXPECT_FALSE(err);
    return b;
}

big::Int intFromHex(std::string_view h) {
    big::Int n;
    n.SetBytes(fromHex(h));
    return n;
}

using Algs = std::vector<std::string_view>;

// GenerateSigningTestKey: the signing key and the verification key of alg.
std::pair<ptr<object>, ptr<object>> signingTestKey(std::string_view alg) {
    const auto& k = keys();
    if (alg == jose::EdDSA) return {k.edPriv, k.edPub};
    if (alg == jose::ES256) return {k.ec256, k.ec256->Public()};
    if (alg == jose::ES384) return {k.ec384, k.ec384->Public()};
    if (alg == jose::ES512) return {k.ec521, k.ec521->Public()};
    return {k.rsa, k.rsa->Public()};
}

const Algs& signatureAlgorithms() {
    static const Algs algs = {jose::RS256, jose::RS384, jose::RS512, jose::PS256, jose::PS384,
                              jose::PS512, jose::ES256, jose::ES384, jose::ES512, jose::EdDSA};
    return algs;
}

// RoundtripJWS: signs, serializes, parses, corrupts and verifies. The
// nonce goes in as an extra header, as go++ has no NonceSource.
error roundtripJWS(std::string_view alg, const std::function<void(jose::JSONWebSignature&)>& corrupter,
                   const ptr<object>& signingKey, const object& verificationKey, std::string_view nonce) {
    jose::SignerOptions opts;
    if (!nonce.empty()) opts.WithHeader("nonce", json::Value(nonce));
    auto [signer, err] = jose::NewSigner({alg, signingKey}, &opts);
    if (err) return errors::New("error on new signer: " + err.Error());

    std::string input = "Lorem ipsum dolor sit amet";
    auto [obj, err2] = signer->Sign(as_bytes(input));
    if (err2) return errors::New("error on sign: " + err2.Error());

    auto [msg, err3] = obj->CompactSerialize();
    if (err3) return errors::New("error on serialize: " + err3.Error());

    std::array<std::string_view, 1> algs{alg};
    auto [parsed, err4] = jose::ParseSigned(msg, algs);
    if (err4) return errors::New("error on parse: " + err4.Error());

    corrupter(*parsed);

    auto [output, err5] = parsed->Verify(verificationKey);
    if (err5) return errors::New("error on verify: " + err5.Error());

    for (const auto& sig : parsed->Signatures) {
        if (sig.Header.JSONWebKey) {
            if (auto [_, e] = parsed->Verify(*sig.Header.JSONWebKey); e) {
                return errors::New("error on verify with embedded key: " + e.Error());
            }
        }
        if (sig.Header.Nonce != nonce) return errors::New("Incorrect nonce returned: [" + sig.Header.Nonce + "]");
    }
    if (str(output) != input) return errors::New("input/output do not match");
    return nullptr;
}

// The keys of TestSampleNimbusJWSMessagesRSA and EC, from their DER.
ptr<rsa::PublicKey> nimbusRSAKey() {
    auto k = make_ptr<rsa::PublicKey>();
    k->N = intFromHex(
        "dda2d21b06de5f4640d876be12f10b688146ccef75f90d7925073fb5d19d0a0196dd701bae1ed95210e1d572b36ecf94390c6a9f712ae18"
        "3b1d7c206d16b094ae09a1d02319c3959b7f18ff162f08cc0b48b39492e6d92c9f33f104a0da664c2df0e8c4367472bb499f59a2afac2a2"
        "8d077e57b09876bf331cca43add34aa04711d078e6bf3cdd3285028619061f94da35b672a259542d38207a969850a4bc42986a7520cc5a2"
        "49eb4a1be83a57cee5bfa0956322b5145ea0ec4b29e4e3a3e69367dff6e617eed5f9bed3bcaf753422255110a313db6846036820fe130e7"
        "5919ffbef7d9dc6ccdaed5a6962ceacf7af929dbc8b3bd5a85680c5120f8c1f6b3af");
    k->E = 65537;
    return k;
}

ptr<ecdsa::PublicKey> ecKey(ptr<elliptic::Curve> c, std::string_view x, std::string_view y) {
    auto k = make_ptr<ecdsa::PublicKey>();
    k->Curve = std::move(c);
    k->X = intFromHex(x);
    k->Y = intFromHex(y);
    return k;
}

std::vector<ptr<ecdsa::PublicKey>> nimbusECKeys() {
    return {
        ecKey(elliptic::P256(), "220eb68eae85c8bd68b448fd529ed2df9054af0185f5356b033aeb635ac750a6",
              "6a60818483aeeefe29a381a75572bef2f50df6234801f16f051ea6eab7af7aac"),
        ecKey(elliptic::P384(),
              "3d7b1596a0ad376a1363e17e845666dccd25758a5bec879e24ce706264f493545aab30450e10cc36760ae50e71f8ecac",
              "b19ac0b478180c5c34d805498619e0fde1d1a7b9aa98bfef237c1bc49b6b2ca6257486c103ef5f60141c29e89b8e5bb9"),
        ecKey(elliptic::P521(),
              "006b6c3730c2791560fab527faf06f96cb98c88565ce0f7200ee9139202fe6c90ce8d6559c0fc292e7b48cea79b83ceb4f731c8b"
              "98384bec52ee87c0caa08672bed0",
              "0074af3eb4e40fed75d1953d9b92fcc8b9ace9d3922ddd300fbb4d18cd322929f0e5735f58d65bdfedeb6b6c5aa14a10fdd81fa1"
              "1e6853e1d76b75b3c20435bc86f5"),
    };
}

const char* const es512Message =
    "eyJhbGciOiJFUzUxMiJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.AeYNFC1rwIgQv-5fwd8iRyYzvTaSCYTEICepgu9gRId-"
    "IW99kbSVY7yH0MvrQnqI-a0L8zwKWDR35fW5dukPAYRkADp3Y1lzqdShFcEFziUVGo46vqbiSajmKFrjBktJcCsfjKSaLHwxErF-"
    "T10YYPCQFHWb2nXJOOI3CZfACYqgO84g";

// A compact message of a protected header, a payload and a signature.
std::string compact(std::string_view protectedHeader, std::string_view payload, std::span<const byte> signature) {
    return b64(protectedHeader) + "." + b64(payload) + "." + b64(signature);
}

// A message signed by key, with the given protected header.
std::string signWith(std::string_view alg, const ptr<object>& key, std::string_view payload,
                     const jose::SignerOptions* opts = nullptr) {
    auto [signer, err] = jose::NewSigner({alg, key}, opts);
    EXPECT_FALSE(err) << err.Error();
    auto [obj, err2] = signer->Sign(as_bytes(payload));
    EXPECT_FALSE(err2) << err2.Error();
    auto [msg, err3] = obj->CompactSerialize();
    EXPECT_FALSE(err3) << err3.Error();
    return std::string(msg);
}

}  // namespace

// ---------------------------------------------------------------------------
// jws_test.go

TEST(GoJWS, ParseEmptySigned) {
    Algs algs{jose::HS256};
    EXPECT_EQ(jose::ParseSigned("", algs).second, errEmptyInput);
    EXPECT_EQ(jose::ParseSignedCompact("", algs).second, errEmptyInput);
}

TEST(GoJWS, CompactParseJWS) {
    Algs algs{"XYZ"};
    // Should parse
    auto [obj, err] = jose::ParseSigned("eyJhbGciOiJYWVoifQ.cGF5bG9hZA.c2lnbmF0dXJl", algs);
    EXPECT_FALSE(err) << err.Error();
    // Should parse (detached signature missing payload)
    std::tie(obj, err) = jose::ParseSigned("eyJhbGciOiJYWVoifQ..c2lnbmF0dXJl", algs);
    EXPECT_FALSE(err) << err.Error();

    // Messages that should fail to parse
    for (std::string_view msg : {
             // Not enough parts
             "eyJhbGciOiJYWVoifQ.cGF5bG9hZA",
             // Invalid signature
             "eyJhbGciOiJYWVoifQ.cGF5bG9hZA.////",
             // Invalid payload
             "eyJhbGciOiJYWVoifQ.////.c2lnbmF0dXJl",
             // Invalid header
             "////.eyJhbGciOiJYWVoifQ.c2lnbmF0dXJl",
             // Invalid header
             "cGF5bG9hZA.cGF5bG9hZA.c2lnbmF0dXJl",
             // Too many parts
             "eyJhbGciOiJYWVoifQ.cGF5bG9hZA.c2lnbmF0dXJl.....................................................",
             "eyJhbGciOiJYWVoifQ.cGF5bG9hZA.c2lnbmF0dXJl.cGF5bG9hZA.cGF5bG9hZA.cGF5bG9hZA....................",
         }) {
        EXPECT_TRUE(jose::ParseSigned(msg, algs).second) << "Able to parse invalid message " << msg;
    }
}

TEST(GoJWS, RejectUnprotectedJWSNonce) {
    // A compact message has no unprotected header, so its nonce is always
    // protected. The JSON serializations that Go checks here are not
    // supported.
    Algs algs{"XYZ"};
    EXPECT_EQ(jose::ParseSigned(R"({
		"header": { "nonce": "should-cause-an-error" },
		"payload": "does-not-matter",
		"signature": "does-not-matter"
	})",
                                algs)
                  .second,
              jose::ErrNotSupported);
    EXPECT_EQ(jose::ParseSigned(R"(  {
		"payload": "does-not-matter",
 		"signatures": [{
 			"header": { "nonce": "should-cause-an-error" },
			"signature": "does-not-matter"
		}]
	})",
                                algs)
                  .second,
              jose::ErrNotSupported);

    auto [obj, err] = jose::ParseSigned(compact(R"({"alg":"XYZ","nonce":"n"})", "p", as_bytes("s")), algs);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(obj->Signatures[0].Header.Nonce, "n");
    EXPECT_EQ(obj->Signatures[0].Protected.Nonce, "n");
    EXPECT_EQ(obj->Signatures[0].Unprotected.Nonce, "");
}

TEST(GoJWS, SampleNimbusJWSMessagesRSA) {
    auto rsaPublicKey = nimbusRSAKey();
    for (std::string_view msg : {
             "eyJhbGciOiJSUzI1NiJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.YHX849fvekz6wJGeyqnQhFqyHFcUXNJKj3o2w3ddR46YLlsCop"
             "UJrlifRU_ZuTWzpYxt5oC--T2eoqMhlCvltSWrE5_1_EumqiMfAYsZULx9E6Jns7q3w7mttonYFSIh7aR3-yg2HMMfTCgoAY1y_AZ4Vj"
             "XwHDcZ5gu1oZDYgvZF4uXtCmwT6e5YtR1m8abiWPF8BgoTG_BD3KV6ClLj_QQiNFdfdxAMDw7vKVOKG1T7BFtz6cDs2Q3ILS4To5E2Ij"
             "cVSSYS8mi77EitCrWmrqbK_G3WCdKeUFGnMnyuKXaCDy_7FLpAZ6Z5RomRr5iskXeJZdZqIKcJV8zl4fpsPA",
             "eyJhbGciOiJSUzM4NCJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.meyfoOTjAAjXHFYiNlU7EEnsYtbeUYeEglK6BL_cxISEr2YAGL"
             "r1Gwnn2HnucTnH6YilyRio7ZC1ohy_ZojzmaljPHqpr8kn1iqNFu9nFE2M16ZPgJi38-PGzppcDNliyzOQO-c7L-eA-v8Gfww5uyRaOJ"
             "diWg-hUJmeGBIngPIeLtSVmhJtz8oTeqeNdUOqQv7f7VRCuvagLhW1PcEM91VUS-gS0WEUXoXWZ2lp91No0v1O24izgX3__FKiX_16Xh"
             "rOfAgJ82F61vjbTIQYwhexHPZyYTlXYt_scNRzFGhSKeGFin4zVdFLOXWJqKWdUd5IrDP5Nya3FSoWbWDXAg",
             "eyJhbGciOiJSUzUxMiJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.rQPz0PDh8KyE2AX6JorgI0MLwv-qi1tcWlz6tuZuWQG1hdrlzq"
             "5tR1tQg1evYNc_SDDX87DWTSKXT7JEqhKoFixLfZa13IJrOc7FB8r5ZLx7OwOBC4F--OWrvxMA9Y3MTJjPN3FemQePUo-na2vNUZv-Yg"
             "kcbuOgbO3hTxwQ7j1JGuqy-YutXOFnccdXvntp3t8zYZ4Mg1It_IyL9pzgGqHIEmMV1pCFGHsDa-wStB4ffmdhrADdYZc0q_SvxUdoby"
             "C_XzZCz9ENzGIhgwYxyyrqg7kjqUGoKmCLmoSlUFW7goTk9IC5SXdUyLPuESxOWNfHoRClGav230GYjPFQFA",
             "eyJhbGciOiJQUzI1NiJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.UTtxjsv_6x4CdlAmZfAW6Lun3byMjJbcwRp_OlPH2W4MZaZar7"
             "aql052mIB_ddK45O9VUz2aphYVRvKPZY8WHmvlTUU30bk0z_cDJRYB9eIJVMOiRCYj0oNkz1iEZqsP0YgngxwuUDv4Q4A6aJ0Bo5E_rZ"
             "o3AnrVHMHUjPp_ZRRSBFs30tQma1qQ0ApK4Gxk0XYCYAcxIv99e78vldVRaGzjEZmQeAVZx4tGcqZP20vG1L84nlhSGnOuZ0FhR8UjRF"
             "LXuob6M7EqtMRoqPgRYw47EI3fYBdeSivAg98E5S8R7R1NJc7ef-l03RvfUSY0S3_zBq_4PlHK6A-2kHb__w",
         }) {
        auto [obj, err] = jose::ParseSigned(msg, Algs{jose::RS256, jose::RS384, jose::RS512, jose::PS256});
        ASSERT_FALSE(err) << "unable to parse message " << msg << ": " << err.Error();
        auto [payload, err2] = obj->Verify(*rsaPublicKey);
        ASSERT_FALSE(err2) << "unable to verify message " << msg << ": " << err2.Error();
        EXPECT_EQ(str(payload), "Lorem ipsum dolor sit amet");
    }
}

TEST(GoJWS, SampleNimbusJWSMessagesEC) {
    auto ecPublicKeys = nimbusECKeys();
    std::vector<std::string_view> ecSampleMessages = {
        "eyJhbGciOiJFUzI1NiJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.MEWJVlvGRQyzMEGOYm4rwuiwxrX-6LjnlbaRDAuhwmnBm2Gtn7pRpG"
        "XRTMFZUXsSGDz2L1p-Hz1qn8j9bFIBtQ",
        "eyJhbGciOiJFUzM4NCJ9.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ.nbdjPnJPYQtVNNdBIx8-KbFKplTxrz-hnW5UNhYUY7SBkwHK4NZnqc"
        "2Lv4DXoA0aWHq9eiypgOh1kmyPWGEmqKAHUx0xdIEkBoHk3ZsbmhOQuq2jL_wcMUG6nTWNhLrB",
        es512Message,
    };
    for (size_t i = 0; i < ecSampleMessages.size(); i++) {
        auto msg = ecSampleMessages[i];
        auto [obj, err] = jose::ParseSigned(msg, Algs{jose::ES256, jose::ES384, jose::ES512});
        ASSERT_FALSE(err) << "unable to parse message " << msg << ": " << err.Error();
        auto [payload, err2] = obj->Verify(*ecPublicKeys[i]);
        ASSERT_FALSE(err2) << "unable to verify message " << msg << ": " << err2.Error();
        EXPECT_EQ(str(payload), "Lorem ipsum dolor sit amet");
    }
}

TEST(GoJWS, HeaderFieldsCompact) {
    auto [obj, err] = jose::ParseSigned(es512Message, Algs{jose::ES512});
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(obj->Signatures[0].Header.Algorithm, "ES512") << "merged header did not contain expected alg value";
    EXPECT_EQ(obj->Signatures[0].Protected.Algorithm, "ES512") << "protected header did not contain expected alg value";
    EXPECT_EQ(obj->Signatures[0].Unprotected.Algorithm, "") << "unprotected header contained an alg value";
}

TEST(GoJWS, ErrorMissingPayloadJWS) {
    rawJSONWebSignature raw;
    Algs algs{jose::RS256};
    auto [obj, err] = raw.sanitized(algs);
    ASSERT_TRUE(err) << "was able to parse message with missing payload";
    EXPECT_TRUE(contains(err, "missing payload")) << err.Error();
}

TEST(GoJWS, ErrorUnexpectedSignatureAlgorithmInProtected) {
    // protected: {"alg":"HS256", "jwk":{"kty":"oct", "k":"MTEx"}}
    std::string msg =
        "eyJhbGciOiJIUzI1NiIsICJqd2siOnsia3R5Ijoib2N0IiwgImsiOiJNVEV4In19.TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ."
        "lvo41ZZsuHwQvSh0uJtEXRR3vmuBJ7in6qMoD7p9jyo";
    auto [obj, err] = jose::ParseSigned(msg, Algs{jose::ES256});
    ASSERT_TRUE(err) << "was able to parse message with unexpected signature algorithm";
    auto* e = errors::As<jose::ErrUnexpectedSignatureAlgorithm>(err);
    ASSERT_NE(e, nullptr) << "unexpected error type: " << err.Error();
    EXPECT_EQ(e->Got, jose::HS256);
    ASSERT_EQ(e->expected.size(), 1u);
    EXPECT_EQ(e->expected[0], jose::ES256);
    EXPECT_EQ(err.Error(), R"(unexpected signature algorithm "HS256"; expected ["ES256"])");

    // The flattened JSON message of Go's test is not supported.
    EXPECT_EQ(jose::ParseSigned(
                  R"({"payload":"TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQ","protected":"eyJhbGciOiJIUzI1NiIsICJqd2siOnsia3R5Ijoib2N0IiwgImsiOiJNVEV4In19","signature":"lvo41ZZsuHwQvSh0uJtEXRR3vmuBJ7in6qMoD7p9jyo"})",
                  Algs{jose::ES256})
                  .second,
              jose::ErrNotSupported);
}

TEST(GoJWS, UnexpectedAlgorithmCheckedBeforeHeaders) {
    // A message with an unexpected algorithm must be rejected as such, before
    // the rest of the header, including any embedded key, is parsed.
    std::string protectedHeader = b64(R"({"alg":"HS256","jwk":"not a key"})");
    std::string payload = b64("Lorem ipsum dolor sit amet");
    std::string signature = b64("not a real signature");
    auto [obj, err] = jose::ParseSigned(protectedHeader + "." + payload + "." + signature, Algs{jose::RS256});
    EXPECT_NE(errors::As<jose::ErrUnexpectedSignatureAlgorithm>(err), nullptr) << err.Error();
    // The same header with an allowed algorithm fails on the key.
    std::tie(obj, err) = jose::ParseSigned(protectedHeader + "." + payload + "." + signature, Algs{jose::HS256});
    EXPECT_TRUE(err);
    EXPECT_EQ(errors::As<jose::ErrUnexpectedSignatureAlgorithm>(err), nullptr);
}

// Test that a null value in the header doesn't panic
TEST(GoJWS, NullHeaderValue) {
    // Go's message is flattened JSON; this is its protected header, compact.
    std::string msg = std::string("eyJhbGciOiJFUzI1NiIsIm5vbmNlIjpudWxsfQ.") +
                      "eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFtcGxlLmNvbS9pc19yb290Ijp0cnVlfQ." +
                      "DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSApmWQxfKTUJqPP3-Kg6NU1Q";
    auto [obj, err] = jose::ParseSigned(msg, Algs{jose::ES256});
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(obj->Signatures[0].Header.Nonce, "");
    // And null for each of the other members.
    for (std::string_view h : {R"({"alg":"ES256","kid":null})", R"({"alg":"ES256","jwk":null})",
                               R"({"alg":"ES256","crit":null})", R"({"alg":"ES256","x":null})"}) {
        std::tie(obj, err) = jose::ParseSigned(compact(h, "p", as_bytes("s")), Algs{jose::ES256});
        EXPECT_FALSE(err) << h << ": " << err.Error();
    }
}

// A protected header of JSON null must be rejected at parse time.
TEST(GoJWS, RejectNullProtectedHeader) {
    for (std::string_view input : {
             // Compact, "null"
             "bnVsbA.dGVzdA.AAAA",
             // Compact, " null "
             "IG51bGwg.dGVzdA.AAAA",
         }) {
        auto [obj, err] = jose::ParseSigned(input, Algs{jose::HS256});
        EXPECT_TRUE(contains(err, "invalid protected header")) << input << ": " << err.Error();
    }
    // The JSON messages of Go's test are not supported.
    EXPECT_EQ(
        jose::ParseSigned(R"({"payload":"dGVzdA","protected":"bnVsbA","header":{"alg":"HS256"},"signature":"AAAA"})",
                          Algs{jose::HS256})
            .second,
        jose::ErrNotSupported);
}

TEST(GoJWS, EmbeddedMalformedEd25519JWK) {
    struct {
        const char* name;
        slice<byte> x;
    } testCases[] = {
        {"short x", slice<byte>{1}},
        {"identity point sign-bit alias", fromHex(R"(
				01000000000000000000000000000000
				00000000000000000000000000000080)")},
    };
    for (const auto& tc : testCases) {
        std::string protectedHeader = R"({"alg":"EdDSA","jwk":{"kty":"OKP","crv":"Ed25519","x":")" + b64(tc.x) + "\"}}";
        auto signature = slice<byte>::make(64);
        signature[0] = 1;
        auto [obj, err] = jose::ParseSigned(compact(protectedHeader, "forged payload", signature), Algs{jose::EdDSA});
        EXPECT_TRUE(err) << tc.name << ": should not allow parsing JWS with malformed Ed25519 embedded JWK";
    }
}

TEST(GoJWS, EmbeddedPrivateJWKRejected) {
    const auto& rsaTestKey = *keys().rsa;
    auto [privateJWK, err] = fromRsaPrivateKey(rsaTestKey);
    ASSERT_FALSE(err);
    auto publicJWK = fromRsaPublicKey(rsaTestKey.PublicKey);
    auto strayJWK = fromRsaPublicKey(rsaTestKey.PublicKey);
    strayJWK.P = privateJWK.P;
    strayJWK.Q = privateJWK.Q;

    struct {
        const char* name;
        std::string jwk;
        bool wantErr;
    } testCases[] = {
        {"public key", str(json::Marshal(publicJWK).first), false},
        {"private key", str(json::Marshal(privateJWK).first), true},
        {"symmetric key", R"({"kty":"oct","k":"MTEx"})", true},
        {"public key with stray private members", str(json::Marshal(strayJWK).first), true},
        {"public key with a stray dp", R"({"kty":"RSA","n":")" + std::string(publicJWK.N->base64()) +
                                           R"(","e":"AQAB","dp":"AQAB"})",
         true},
    };
    for (const auto& tc : testCases) {
        std::string msg = compact(R"({"alg":"RS256","jwk":)" + tc.jwk + "}", "Lorem ipsum dolor sit amet",
                                  as_bytes("not a real signature"));
        auto [obj, err2] = jose::ParseSigned(msg, Algs{jose::RS256});
        if (tc.wantErr) {
            EXPECT_TRUE(contains(err2, "JWK contains private parameters")) << tc.name << ": " << err2.Error();
        } else {
            EXPECT_FALSE(err2) << tc.name << ": " << err2.Error();
        }
    }
}

// ---------------------------------------------------------------------------
// signing_test.go

TEST(GoJWS, RoundtripsJWS) {
    for (auto alg : signatureAlgorithms()) {
        auto [signingKey, verificationKey] = signingTestKey(alg);
        error err = roundtripJWS(alg, [](jose::JSONWebSignature&) {}, signingKey, *verificationKey, "test_nonce");
        EXPECT_FALSE(err) << alg << ": " << err.Error();
    }
    // HMAC: there are no symmetric keys.
    for (std::string_view alg : {jose::HS256, jose::HS384, jose::HS512}) {
        auto [signer, err] = jose::NewSigner({alg, keys().rsa}, nullptr);
        EXPECT_EQ(err, jose::ErrUnsupportedAlgorithm) << alg;
    }
}

TEST(GoJWS, RoundtripsJWSCorruptSignature) {
    std::vector<std::function<void(jose::JSONWebSignature&)>> corrupters = {
        // Changes bytes in signature
        [](jose::JSONWebSignature& obj) { obj.Signatures[0].signature[10]++; },
        // Set totally invalid signature
        [](jose::JSONWebSignature& obj) { obj.Signatures[0].signature = toBytes("###"); },
    };
    for (auto alg : signatureAlgorithms()) {
        auto [signingKey, verificationKey] = signingTestKey(alg);
        for (size_t j = 0; j < corrupters.size(); j++) {
            error err = roundtripJWS(alg, corrupters[j], signingKey, *verificationKey, "test_nonce");
            EXPECT_TRUE(err) << "failed to detect corrupt signature " << alg << " " << j;
        }
    }
}

TEST(GoJWS, JWSInvalidKey) {
    auto [signingKey0, verificationKey0] = signingTestKey(jose::RS256);
    auto verificationKey1 = signingTestKey(jose::ES256).second;
    auto verificationKey2 = signingTestKey(jose::EdDSA).second;

    auto [signer, err] = jose::NewSigner({jose::RS256, signingKey0}, nullptr);
    ASSERT_FALSE(err);
    auto [obj, err2] = signer->Sign(as_bytes("Lorem ipsum dolor sit amet"));
    ASSERT_FALSE(err2);

    // Must work with correct key
    EXPECT_FALSE(obj->Verify(*verificationKey0).second);
    // Must not work with incorrect key
    EXPECT_TRUE(obj->Verify(*verificationKey1).second);
    EXPECT_TRUE(obj->Verify(*verificationKey2).second);
    // Must not work with invalid key: Go's is a string; a private key is no
    // verification key either.
    EXPECT_EQ(obj->Verify(*signingKey0).second, jose::ErrUnsupportedKeyType);
    EXPECT_EQ(obj->Verify(jose::JSONWebKey{}).second, jose::ErrUnsupportedKeyType);
}

TEST(GoJWS, JWSNilVerificationKey) {
    // Go's nil keys; here the empty ones.
    struct {
        const char* name;
        std::string_view algorithm;
        ptr<object> verificationKey;
    } tests[] = {
        {"Ed25519", jose::EdDSA, make_ptr<ed25519::PublicKey>()},
        {"ECDSA", jose::ES256, make_ptr<ecdsa::PublicKey>()},
        {"RSA", jose::RS256, make_ptr<rsa::PublicKey>()},
    };
    for (const auto& test : tests) {
        auto signingKey = signingTestKey(test.algorithm).first;
        auto [signer, err] = jose::NewSigner({test.algorithm, signingKey}, nullptr);
        ASSERT_FALSE(err);
        auto [obj, err2] = signer->Sign(as_bytes("payload"));
        ASSERT_FALSE(err2);
        EXPECT_EQ(obj->Verify(*test.verificationKey).second, errInvalidVerificationKey) << test.name;
        jose::JSONWebKey jwk;
        jwk.Key = test.verificationKey;
        EXPECT_EQ(obj->Verify(jwk).second, errInvalidVerificationKey) << test.name;
    }
}

TEST(GoJWS, InvalidSignerAlg) {
    EXPECT_TRUE(jose::NewSigner({"XYZ", nullptr}, nullptr).second) << "should not accept invalid algorithm";
    EXPECT_TRUE(jose::NewSigner({"XYZ", keys().rsa}, nullptr).second) << "should not accept invalid algorithm";
}

TEST(GoJWS, InvalidJWS) {
    const auto& rsaTestKey = *keys().rsa;
    auto [signer, err] = jose::NewSigner({jose::PS256, keys().rsa}, nullptr);
    ASSERT_FALSE(err);
    auto [obj, err2] = signer->Sign(as_bytes("Lorem ipsum dolor sit amet"));
    ASSERT_FALSE(err2);
    EXPECT_FALSE(obj->Verify(rsaTestKey.PublicKey).second);

    // headerB64 is known, but should be in protected
    obj->Signatures[0].header_ = rawHeader{};
    ASSERT_FALSE(setHeader(*obj->Signatures[0].header_, "crit", std::vector<std::string>{"b64"}));
    EXPECT_TRUE(obj->Verify(rsaTestKey.PublicKey).second)
        << "should not verify message with an unprotected but known crit header";

    // reject unknown critical headers
    obj->Signatures[0].protected_ = rawHeader{};
    obj->Signatures[0].header_ = rawHeader{};
    ASSERT_FALSE(setHeader(*obj->Signatures[0].protected_, "crit", std::vector<std::string>{"unknown-critical-header"}));
    EXPECT_TRUE(obj->Verify(rsaTestKey.PublicKey).second) << "should not verify message with unknown crit header";

    // Try without alg header
    obj->Signatures[0].protected_ = rawHeader{};
    obj->Signatures[0].header_ = rawHeader{};
    EXPECT_TRUE(obj->Verify(rsaTestKey.PublicKey).second) << "should not verify message with missing headers";
}

TEST(GoJWS, SignerKid) {
    std::string kid = "DEADBEEF";
    std::string payload = "Lorem ipsum dolor sit amet";

    auto key = ecdsa::GenerateKey(elliptic::P256(), nullptr).first;
    jose::JSONWebKey basejwk;
    basejwk.Key = key;
    auto [jsonbar, err] = basejwk.MarshalJSON();
    ASSERT_FALSE(err);

    std::map<string, json::Value> jsonmsi;
    ASSERT_FALSE(json::Unmarshal(jsonbar, &jsonmsi));
    jsonmsi["kid"] = json::Value(std::string_view(kid));
    auto [jsonbar2, err2] = json::Marshal(jsonmsi);
    ASSERT_FALSE(err2);

    auto jwk = make_ptr<jose::JSONWebKey>();
    ASSERT_FALSE(jwk->UnmarshalJSON(jsonbar2));

    std::string serialized = signWith(jose::ES256, jwk, payload);
    auto [parsed, err3] = jose::ParseSigned(serialized, Algs{jose::ES256});
    ASSERT_FALSE(err3) << err3.Error();
    EXPECT_EQ(parsed->Signatures[0].Header.KeyID, kid) << "KeyID did not survive trip";

    // Verify with JWKS
    jose::JSONWebKeySet set;
    set.Keys = append(set.Keys, jwk->Public());
    auto [out, err4] = parsed->Verify(set);
    EXPECT_FALSE(err4) << "problem verifying with JWKS: " << err4.Error();
    EXPECT_EQ(str(out), payload);
}

TEST(GoJWS, EmbedJwk) {
    std::string payload = "Lorem ipsum dolor sit amet";
    auto key = ecdsa::GenerateKey(elliptic::P256(), nullptr).first;

    jose::SignerOptions opts;
    opts.EmbedJWK = true;
    auto [object, err] = jose::ParseSigned(signWith(jose::ES256, key, payload, &opts), Algs{jose::ES256});
    ASSERT_FALSE(err) << err.Error();
    auto jwk = object->Signatures[0].Protected.JSONWebKey;
    ASSERT_TRUE(jwk) << "JWK isn't set in protected header";
    EXPECT_TRUE(jwk->IsPublic());
    EXPECT_EQ(object->Signatures[0].Protected.KeyID, "");
    EXPECT_FALSE(object->Verify(*jwk).second);

    // This time, sign and do not embed JWK in message
    opts.EmbedJWK = false;
    std::tie(object, err) = jose::ParseSigned(signWith(jose::ES256, key, payload, &opts), Algs{jose::ES256});
    ASSERT_FALSE(err) << err.Error();
    EXPECT_FALSE(object->Signatures[0].Protected.JSONWebKey) << "JWK is set in protected header";
}

TEST(GoJWS, SignerOptionsEd) {
    auto key = signingTestKey(jose::EdDSA).first;
    jose::SignerOptions opts;
    opts.EmbedJWK = true;
    opts.WithHeader("cty", "JWT");
    opts.WithType("JWT");
    auto [sig, err] = jose::NewSigner({jose::EdDSA, key}, &opts);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_TRUE(opts == sig->Options()) << "Signer options do not match";
}

TEST(GoJWS, SignerOptions) {
    // Go's signs with HS256; there are no symmetric keys here.
    auto key = signingTestKey(jose::ES256).first;
    jose::SignerOptions opts;
    opts.EmbedJWK = true;
    opts.WithHeader("cty", "JWT").WithType("JWT");
    auto [sig, err] = jose::NewSigner({jose::ES256, key}, &opts);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_TRUE(opts == sig->Options()) << "Signer options do not match";
    EXPECT_EQ(opts.ExtraHeaders.size(), 2u);
    EXPECT_EQ(opts.ExtraHeaders["typ"], json::Value("JWT"));
}

// Test that extra headers are generated and parsed in a round trip.
TEST(GoJWS, SignerExtraHeaderInclusion) {
    auto key = ecdsa::GenerateKey(elliptic::P256(), nullptr).first;
    jose::SignerOptions opts;
    opts.WithHeader("cty", "foo/bar").WithHeader("myCustomHeader", "xyz");
    auto [object, err] =
        jose::ParseSigned(signWith(jose::ES256, key, "Lorem ipsum dolor sit amet", &opts), Algs{jose::ES256});
    ASSERT_FALSE(err) << err.Error();
    std::map<string, json::Value> correct = {{"cty", "foo/bar"}, {"myCustomHeader", "xyz"}};
    EXPECT_TRUE(object->Signatures[0].Header.ExtraHeaders == correct) << "Mismatch in extra headers";
}

// ---------------------------------------------------------------------------
// asymmetric_test.go

TEST(GoJWS, Ed25519) {
    EXPECT_EQ(newEd25519Signer("XYZ", nullptr).second, jose::ErrUnsupportedAlgorithm);

    edEncrypterVerifier enc;
    enc.publicKey = keys().edPub.get();
    EXPECT_EQ(enc.verifyPayload({}, {}, "XYZ"), jose::ErrUnsupportedAlgorithm);

    edDecrypterSigner dec;
    dec.privateKey = keys().edPriv;
    EXPECT_EQ(dec.signPayload({}, "XYZ").second, jose::ErrUnsupportedAlgorithm);

    auto [sig, err] = dec.signPayload(as_bytes("This is a test"), "EdDSA");
    ASSERT_FALSE(err) << "should not error trying to sign payload";
    EXPECT_EQ(sig.signature.size(), 64u);
    EXPECT_FALSE(enc.verifyPayload(as_bytes("This is a test"), sig.signature, "EdDSA"));
    EXPECT_TRUE(enc.verifyPayload(as_bytes("This is test number 2"), sig.signature, "EdDSA"));

    // Go's ed25519.Verify panics on a public key of the wrong size; here it
    // fails.
    ed25519::PublicKey shortKey;
    shortKey.key = slice<byte>{1, 2, 3};
    enc.publicKey = &shortKey;
    EXPECT_TRUE(enc.verifyPayload(as_bytes("This is a test"), sig.signature, "EdDSA"));
    // And through Verify, of a key a byte short and a byte long.
    std::string msg = signWith(jose::EdDSA, keys().edPriv, "This is a test");
    auto [obj, err2] = jose::ParseSigned(msg, Algs{jose::EdDSA});
    ASSERT_FALSE(err2);
    for (size_t n : {31u, 33u}) {
        ed25519::PublicKey k;
        k.key = slice<byte>::make(n);
        std::copy_n(keys().edPub->key.begin(), std::min<size_t>(n, 32), k.key.begin());
        EXPECT_EQ(obj->Verify(k).second, errInvalidVerificationKey) << n;
    }
    EXPECT_FALSE(obj->Verify(*keys().edPub).second);
}

TEST(GoJWS, InvalidAlgorithmsRSA) {
    EXPECT_EQ(newRSASigner("XYZ", nullptr).second, jose::ErrUnsupportedAlgorithm);

    rsaEncrypterVerifier enc;
    enc.publicKey = &keys().rsa->PublicKey;
    EXPECT_EQ(enc.verifyPayload({}, {}, "XYZ"), jose::ErrUnsupportedAlgorithm);

    rsaDecrypterSigner dec;
    dec.privateKey = keys().rsa;
    EXPECT_EQ(dec.signPayload({}, "XYZ").second, jose::ErrUnsupportedAlgorithm);
}

TEST(GoJWS, InvalidAlgorithmsEC) {
    EXPECT_EQ(newECDSASigner("XYZ", nullptr).second, jose::ErrUnsupportedAlgorithm);
}

TEST(GoJWS, InvalidECPublicKey) {
    // Go's checks an ECDH key; this checks the same invalid key, a point
    // that is not on the curve, as a verification key.
    auto invalid = make_ptr<ecdsa::PublicKey>();
    invalid->Curve = elliptic::P256();
    invalid->X.SetBytes(as_bytes("111"));
    invalid->Y.SetBytes(as_bytes("111"));

    ecDecrypterSigner dec;
    dec.privateKey = keys().ec256;
    auto [sig, err] = dec.signPayload(as_bytes("payload"), "ES256");
    ASSERT_FALSE(err);
    ecEncrypterVerifier enc;
    enc.publicKey = invalid.get();
    EXPECT_TRUE(enc.verifyPayload(as_bytes("payload"), sig.signature, "ES256"));
    enc.publicKey = &keys().ec256->PublicKey;
    EXPECT_FALSE(enc.verifyPayload(as_bytes("payload"), sig.signature, "ES256"));
}

TEST(GoJWS, InvalidAlgorithmEC) {
    ecEncrypterVerifier enc;
    enc.publicKey = &keys().ec256->PublicKey;
    EXPECT_EQ(enc.verifyPayload({}, {}, "XYZ"), jose::ErrUnsupportedAlgorithm);
}

// ---------------------------------------------------------------------------
// Messages an attacker made.

TEST(GoJWS, RejectsAlgNone) {
    Algs all{jose::RS256, jose::RS384, jose::RS512, jose::PS256, jose::PS384, jose::PS512, jose::ES256,
             jose::ES384, jose::ES512, jose::EdDSA, jose::HS256, jose::HS384, jose::HS512};
    std::string msg = compact(R"({"alg":"none"})", R"({"sub":"admin"})", {});
    EXPECT_TRUE(jose::ParseSigned(msg, all).second);
    EXPECT_TRUE(jose::ParseSigned(b64(R"({"alg":"none"})") + "." + b64(R"({"sub":"admin"})") + ".", all).second);

    // Even when the caller allows it, no key verifies it.
    all.push_back("none");
    auto [obj, err] = jose::ParseSigned(msg, all);
    ASSERT_FALSE(err) << err.Error();
    for (auto alg : signatureAlgorithms()) {
        EXPECT_EQ(obj->Verify(*signingTestKey(alg).second).second, jose::ErrCryptoFailure) << alg;
    }
}

TEST(GoJWS, RejectsHS256WithRSAKey) {
    // The classic confusion: the RSA public key's bytes as an HMAC secret.
    const auto& pub = keys().rsa->PublicKey;
    jose::JSONWebKey pubJWK;
    pubJWK.Key = keys().rsa->Public();
    std::string signingInput = b64(R"({"alg":"HS256"})") + "." + b64(R"({"sub":"admin"})");
    for (auto secret : {pub.N.Bytes(), pubJWK.MarshalJSON().first}) {
        auto mac = hmac::New(sha256::New, secret);
        mac->Write(as_bytes(signingInput));
        std::string msg = signingInput + "." + b64(mac->Sum(slice<byte>()));

        auto [obj, err] = jose::ParseSigned(msg, Algs{jose::HS256});
        ASSERT_FALSE(err) << err.Error();
        EXPECT_EQ(obj->Verify(pub).second, jose::ErrCryptoFailure);
        EXPECT_EQ(obj->Verify(pubJWK).second, jose::ErrCryptoFailure);
    }
    // Signing with HMAC is not supported either.
    EXPECT_EQ(jose::NewSigner({jose::HS256, keys().rsa}, nullptr).second, jose::ErrUnsupportedAlgorithm);
}

TEST(GoJWS, KeyMustFitAlgorithm) {
    std::string payload = "payload";
    struct {
        std::string_view alg;
        ptr<object> wrongKey;
    } cases[] = {
        {jose::RS256, keys().ec256->Public()},  {jose::RS256, keys().edPub},
        {jose::PS256, keys().ec256->Public()},  {jose::ES256, keys().rsa->Public()},
        {jose::ES256, keys().ec384->Public()},  {jose::ES384, keys().ec256->Public()},
        {jose::ES512, keys().ec384->Public()},  {jose::ES256, keys().edPub},
        {jose::EdDSA, keys().rsa->Public()},    {jose::EdDSA, keys().ec256->Public()},
    };
    for (const auto& c : cases) {
        std::string msg = signWith(c.alg, signingTestKey(c.alg).first, payload);
        auto [obj, err] = jose::ParseSigned(msg, Algs{c.alg});
        ASSERT_FALSE(err) << err.Error();
        EXPECT_EQ(obj->Verify(*c.wrongKey).second, jose::ErrCryptoFailure) << c.alg;
        jose::JSONWebKey jwk;
        jwk.Key = c.wrongKey;
        EXPECT_EQ(obj->Verify(jwk).second, jose::ErrCryptoFailure) << c.alg;
    }
    // The alg of an RSA signature changed to the other RSA algorithms.
    std::string msg = signWith(jose::RS256, keys().rsa, payload);
    std::string rest = msg.substr(msg.find('.'));
    for (std::string_view alg : {jose::PS256, jose::RS384}) {
        auto [obj, err] = jose::ParseSigned(b64(R"({"alg":")" + std::string(alg) + R"("})") + rest, Algs{alg});
        ASSERT_FALSE(err);
        EXPECT_EQ(obj->Verify(keys().rsa->PublicKey).second, jose::ErrCryptoFailure) << alg;
    }
}

TEST(GoJWS, ECDSASignatureMustBeFixedWidth) {
    std::string payload = "payload";
    std::string msg = signWith(jose::ES256, keys().ec256, payload);
    auto [obj, err] = jose::ParseSigned(msg, Algs{jose::ES256});
    ASSERT_FALSE(err);
    const auto& pub = keys().ec256->PublicKey;
    ASSERT_FALSE(obj->Verify(pub).second);
    auto good = obj->Signatures[0].signature;
    ASSERT_EQ(good.size(), 64u);

    // A DER signature, a leading zero more or less, and cut.
    std::vector<slice<byte>> bad;
    auto der = ecdsa::SignASN1(nullptr, *keys().ec256, crypto::New(crypto::Hash::SHA256)->Sum(slice<byte>())).first;
    bad.push_back(der);
    auto longer = slice<byte>::make(65);
    std::copy(good.begin(), good.end(), longer.begin() + 1);
    bad.push_back(longer);
    // r, then s with a zero in front: the same integers, but not the size.
    auto paddedS = slice<byte>::make(65);
    std::copy(good.begin(), good.begin() + 32, paddedS.begin());
    std::copy(good.begin() + 32, good.end(), paddedS.begin() + 33);
    bad.push_back(paddedS);
    bad.push_back(good.Sub(1));
    bad.push_back(good.Sub(0, 63));
    bad.push_back(slice<byte>());
    for (const auto& sig : bad) {
        obj->Signatures[0].signature = sig;
        EXPECT_EQ(obj->Verify(pub).second, jose::ErrCryptoFailure) << sig.size();
    }
}

TEST(GoJWS, KeySetKeyMustFitAlgorithm) {
    // The key of the kid in the set is an EC key, the message is RS256.
    std::string payload = "payload";
    jose::SignerOptions opts;
    std::string msg = [&] {
        auto jwk = make_ptr<jose::JSONWebKey>();
        jwk->Key = keys().rsa;
        jwk->KeyID = "k1";
        return signWith(jose::RS256, jwk, payload, &opts);
    }();
    auto [obj, err] = jose::ParseSigned(msg, Algs{jose::RS256});
    ASSERT_FALSE(err);
    ASSERT_EQ(obj->Signatures[0].Header.KeyID, "k1");

    jose::JSONWebKeySet set;
    jose::JSONWebKey ec;
    ec.Key = keys().ec256->Public();
    ec.KeyID = "k1";
    set.Keys = append(set.Keys, ec);
    EXPECT_EQ(obj->Verify(set).second, jose::ErrCryptoFailure);
    // As the first key of the kid is used, another one after it isn't.
    jose::JSONWebKey rsaKey;
    rsaKey.Key = keys().rsa->Public();
    rsaKey.KeyID = "k1";
    set.Keys = append(set.Keys, rsaKey);
    EXPECT_EQ(obj->Verify(set).second, jose::ErrCryptoFailure);
    // The right key alone verifies; through Verify(object&) as well.
    set.Keys = slice<jose::JSONWebKey>{rsaKey};
    EXPECT_FALSE(obj->Verify(set).second);
    EXPECT_FALSE(obj->Verify(static_cast<const object&>(set)).second);
    // No kid, or no key of the kid.
    set.Keys = slice<jose::JSONWebKey>{ec};
    set.Keys[0].KeyID = "other";
    EXPECT_EQ(obj->Verify(set).second, jose::ErrJWKSKidNotFound);
}

TEST(GoJWS, EmbeddedKeyIsNotTrusted) {
    // An attacker signs with their key and embeds it; verification with the
    // trusted key fails.
    std::string payload = R"({"sub":"admin"})";
    auto attacker = ecdsa::GenerateKey(elliptic::P256(), nullptr).first;
    jose::SignerOptions opts;
    opts.EmbedJWK = true;
    std::string msg = signWith(jose::ES256, attacker, payload, &opts);
    auto [obj, err] = jose::ParseSigned(msg, Algs{jose::ES256});
    ASSERT_FALSE(err);
    ASSERT_TRUE(obj->Signatures[0].Header.JSONWebKey);
    EXPECT_EQ(obj->Verify(keys().ec256->PublicKey).second, jose::ErrCryptoFailure);
    jose::JSONWebKeySet set;
    EXPECT_EQ(obj->Verify(set).second, jose::ErrJWKSKidNotFound);
}

TEST(GoJWS, VerifyWithoutSignatures) {
    jose::JSONWebSignature obj;
    EXPECT_TRUE(obj.Verify(keys().rsa->PublicKey).second);
    EXPECT_EQ(obj.CompactSerialize().second, jose::ErrNotSupported);
}

TEST(GoJWS, ParseSignedStripsUnicodeSpace) {
    // ParseSigned strips the white space of unicode.IsSpace; ParseSignedCompact
    // doesn't.
    std::string msg = es512Message;
    std::string spaced = "  " + msg.substr(0, 10) + " \n" + msg.substr(10, 30) + "　\t" + msg.substr(40) +
                         "\u0085";
    auto [obj, err] = jose::ParseSigned(spaced, Algs{jose::ES512});
    ASSERT_FALSE(err) << err.Error();
    EXPECT_FALSE(obj->Verify(*nimbusECKeys()[2]).second);
    EXPECT_TRUE(jose::ParseSignedCompact(spaced, Algs{jose::ES512}).second);
    // Not white space: U+200B.
    EXPECT_TRUE(jose::ParseSigned(msg.substr(0, 10) + "​" + msg.substr(10), Algs{jose::ES512}).second);
    // The strip function itself, with an invalid byte, which Go turns into
    // U+FFFD.
    EXPECT_EQ(stripWhitespace("a b c\xff"), "abc\xef\xbf\xbd");
    EXPECT_EQ(stripWhitespace(" a\n b\t\r c "), "abc");
}
