// Ported from github.com/go-jose/go-jose/v4's jwk_test.go (v4.1.7; Apache
// 2.0, see x/jose/LICENSE), with the names of its tests: TestCurveSize,
// TestRoundtripRsaPrivate, TestRsaPrivateInsufficientPrimes,
// TestRsaPrivateExcessPrimes, TestRsaKeyChecks, TestRoundtripEcPublic,
// TestRoundtripEcPrivate, TestMarshalUnmarshal, TestMarshalNonPointer,
// TestMarshalUnmarshalInvalid, TestWebKeyVectorsInvalid,
// TestWebKeyVectorsValid, TestJWKUnsupported, TestEd25519Serialization,
// TestEd25519RejectsMalformedPublicJWK, TestEd25519RejectsMalformedPrivateJWK,
// TestThumbprint, TestMarshalUnmarshalJWKSet, TestJWKSetKey,
// TestUnmarshalPrivateJWKAllowed, TestJWKIsPublic, TestJWKValid,
// TestJWKBufferSizeCheck, TestJWKPaddingPrivateX, Y and D, and TestJWKPaddingX
// and Y.
//
// Left out: the X.509 tests (TestRoundtripX509, TestRoundtripX509Hex,
// TestCertificatesURL, TestInvalidThumbprintsX509, TestKeyMismatchX509,
// TestKeyMismatchX509UnsupportedCertKey), the symmetric key tests
// (TestJWKSymmetric*), TestRoundtripRsaPrivatePrecomputed (go++'s
// rsa::PrivateKey has no Precomputed), and the OpaqueSigner part of
// TestThumbprint. The last cookbook key has its x5c, which go++ ignores.
// The tests after TestUnmarshalPrivateJWKAllowed's group that Go's suite
// lacks (GoJWK.Untrusted*, KeySet*) pin what go++ does with JWKS input that
// an attacker controls: wrong sizes, points off the curve, and keys that
// are not supported.
#include <gopp/crypto.hpp>
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rsa.hpp>
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

bool contains(const error& err, std::string_view sub) {
    return err && std::string_view(err.Error()).find(sub) != std::string_view::npos;
}

slice<byte> fromHex(std::string_view h) {
    auto [b, err] = hex::DecodeString(stripWhitespace(h));
    EXPECT_FALSE(err);
    return b;
}

// unmarshal: the JWK of s, and the error.
std::pair<jose::JSONWebKey, error> unmarshal(std::string_view s) {
    jose::JSONWebKey jwk;
    error err = jwk.UnmarshalJSON(as_bytes(s));
    return {jwk, err};
}

// An odd integer of exactly the given bit length.
slice<byte> oddOfBits(int bits) {
    auto b = slice<byte>::make((bits + 7) / 8);
    b[0] = static_cast<byte>(1u << ((bits - 1) % 8));
    b[b.size() - 1] |= 1;
    return b;
}

struct ModulusLimit {
    int old = jose::MaxRSAModulusBits;
    ~ModulusLimit() { jose::MaxRSAModulusBits = old; }
};

std::string rawEd25519JWK(std::span<const byte> x, const slice<byte>* d) {
    std::string out = R"({"kty":"OKP","crv":"Ed25519","x":")" + std::string(base64::RawURLEncoding.EncodeToString(x)) + "\"";
    if (d) out += R"(,"d":")" + std::string(base64::RawURLEncoding.EncodeToString(*d)) + "\"";
    return out + "}";
}

// Test vectors from RFC 7520.
const std::vector<std::string>& cookbookJWKs() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> raw = {
    R"JWK({
     "kty": "EC",
     "kid": "bilbo.baggins@hobbiton.example",
     "use": "sig",
     "crv": "P-521",
     "x": "AHKZLLOsCOzz5cY97ewNUajB957y-C-U88c3v13nmGZx6sYl_oJXu9
         A5RkTKqjqvjyekWF-7ytDyRXYgCF5cj0Kt",
     "y": "AdymlHvOiLxXkEhayXQnNCvDX4h9htZaCJN34kfmC6pV5OhQHiraVy
         SsUdaQkAgDPrwQrJmbnX9cwlGfP-HqHZR1"
   })JWK",
    R"JWK({
     "kty": "OKP",
     "crv": "Ed25519",
     "x": "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo",
     "d": "nWGxne_9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A"
   })JWK",
    R"JWK({
     "kty": "EC",
     "kid": "bilbo.baggins@hobbiton.example",
     "use": "sig",
     "crv": "P-521",
     "x": "AHKZLLOsCOzz5cY97ewNUajB957y-C-U88c3v13nmGZx6sYl_oJXu9
           A5RkTKqjqvjyekWF-7ytDyRXYgCF5cj0Kt",
     "y": "AdymlHvOiLxXkEhayXQnNCvDX4h9htZaCJN34kfmC6pV5OhQHiraVy
           SsUdaQkAgDPrwQrJmbnX9cwlGfP-HqHZR1",
     "d": "AAhRON2r9cqXX1hg-RoI6R1tX5p2rUAYdmpHZoC1XNM56KtscrX6zb
           KipQrCW9CGZH3T4ubpnoTKLDYJ_fF3_rJt"
   })JWK",
    R"JWK({
     "kty": "RSA",
     "kid": "bilbo.baggins@hobbiton.example",
     "use": "sig",
     "n": "n4EPtAOCc9AlkeQHPzHStgAbgs7bTZLwUBZdR8_KuKPEHLd4rHVTeT
         -O-XV2jRojdNhxJWTDvNd7nqQ0VEiZQHz_AJmSCpMaJMRBSFKrKb2wqV
         wGU_NsYOYL-QtiWN2lbzcEe6XC0dApr5ydQLrHqkHHig3RBordaZ6Aj-
         oBHqFEHYpPe7Tpe-OfVfHd1E6cS6M1FZcD1NNLYD5lFHpPI9bTwJlsde
         3uhGqC0ZCuEHg8lhzwOHrtIQbS0FVbb9k3-tVTU4fg_3L_vniUFAKwuC
         LqKnS2BYwdq_mzSnbLY7h_qixoR7jig3__kRhuaxwUkRz5iaiQkqgc5g
         HdrNP5zw",
     "e": "AQAB"
   })JWK",
    R"JWK({"kty":"RSA",
      "kid":"juliet@capulet.lit",
      "use":"enc",
      "n":"t6Q8PWSi1dkJj9hTP8hNYFlvadM7DflW9mWepOJhJ66w7nyoK1gPNqFMSQRy
           O125Gp-TEkodhWr0iujjHVx7BcV0llS4w5ACGgPrcAd6ZcSR0-Iqom-QFcNP
           8Sjg086MwoqQU_LYywlAGZ21WSdS_PERyGFiNnj3QQlO8Yns5jCtLCRwLHL0
           Pb1fEv45AuRIuUfVcPySBWYnDyGxvjYGDSM-AqWS9zIQ2ZilgT-GqUmipg0X
           OC0Cc20rgLe2ymLHjpHciCKVAbY5-L32-lSeZO-Os6U15_aXrk9Gw8cPUaX1
           _I8sLGuSiVdt3C_Fn2PZ3Z8i744FPFGGcG1qs2Wz-Q",
      "e":"AQAB",
      "d":"GRtbIQmhOZtyszfgKdg4u_N-R_mZGU_9k7JQ_jn1DnfTuMdSNprTeaSTyWfS
           NkuaAwnOEbIQVy1IQbWVV25NY3ybc_IhUJtfri7bAXYEReWaCl3hdlPKXy9U
           vqPYGR0kIXTQRqns-dVJ7jahlI7LyckrpTmrM8dWBo4_PMaenNnPiQgO0xnu
           ToxutRZJfJvG4Ox4ka3GORQd9CsCZ2vsUDmsXOfUENOyMqADC6p1M3h33tsu
           rY15k9qMSpG9OX_IJAXmxzAh_tWiZOwk2K4yxH9tS3Lq1yX8C1EWmeRDkK2a
           hecG85-oLKQt5VEpWHKmjOi_gJSdSgqcN96X52esAQ",
      "p":"2rnSOV4hKSN8sS4CgcQHFbs08XboFDqKum3sc4h3GRxrTmQdl1ZK9uw-PIHf
           QP0FkxXVrx-WE-ZEbrqivH_2iCLUS7wAl6XvARt1KkIaUxPPSYB9yk31s0Q8
           UK96E3_OrADAYtAJs-M3JxCLfNgqh56HDnETTQhH3rCT5T3yJws",
      "q":"1u_RiFDP7LBYh3N4GXLT9OpSKYP0uQZyiaZwBtOCBNJgQxaj10RWjsZu0c6I
           edis4S7B_coSKB0Kj9PaPaBzg-IySRvvcQuPamQu66riMhjVtG6TlV8CLCYK
           rYl52ziqK0E_ym2QnkwsUX7eYTB7LbAHRK9GqocDE5B0f808I4s",
      "dp":"KkMTWqBUefVwZ2_Dbj1pPQqyHSHjj90L5x_MOzqYAJMcLMZtbUtwKqvVDq3
           tbEo3ZIcohbDtt6SbfmWzggabpQxNxuBpoOOf_a_HgMXK_lhqigI4y_kqS1w
           Y52IwjUn5rgRrJ-yYo1h41KR-vz2pYhEAeYrhttWtxVqLCRViD6c",
      "dq":"AvfS0-gRxvn0bwJoMSnFxYcK1WnuEjQFluMGfwGitQBWtfZ1Er7t1xDkbN9
           GQTB9yqpDoYaN06H7CFtrkxhJIBQaj6nkF5KKS3TQtQ5qCzkOkmxIe3KRbBy
           mXxkb5qwUpX5ELD5xFc6FeiafWYY63TmmEAu_lRFCOJ3xDea-ots",
      "qi":"lSQi-w9CpyUReMErP1RsBLk7wNtOvs5EQpPqmuMvqW57NBUczScEoPwmUqq
           abu9V0-Py4dQ57_bapoKRu1R90bvuFnU63SHWEFglZQvJDMeAvmj4sm-Fp0o
           Yu_neotgQ0hzbI5gry7ajdYy9-2lNx_76aBZoOUu9HCJ-UsfSOI8"})JWK",
    R"JWK({"kty":"RSA",
      "use":"sig",
      "kid":"1b94c",
      "n":"vrjOfz9Ccdgx5nQudyhdoR17V-IubWMeOZCwX_jj0hgAsz2J_pqYW08
           PLbK_PdiVGKPrqzmDIsLI7sA25VEnHU1uCLNwBuUiCO11_-7dYbsr4iJmG0Q
           u2j8DsVyT1azpJC_NG84Ty5KKthuCaPod7iI7w0LK9orSMhBEwwZDCxTWq4a
           YWAchc8t-emd9qOvWtVMDC2BXksRngh6X5bUYLy6AyHKvj-nUy1wgzjYQDwH
           MTplCoLtU-o-8SNnZ1tmRoGE9uJkBLdh5gFENabWnU5m1ZqZPdwS-qo-meMv
           VfJb6jJVWRpl2SUtCnYG2C32qvbWbjZ_jBPD5eunqsIo1vQ",
      "e":"AQAB",
      "x5c":
           ["MIIDQjCCAiqgAwIBAgIGATz/FuLiMA0GCSqGSIb3DQEBBQUAMGIxCzAJB
           gNVBAYTAlVTMQswCQYDVQQIEwJDTzEPMA0GA1UEBxMGRGVudmVyMRwwGgYD
           VQQKExNQaW5nIElkZW50aXR5IENvcnAuMRcwFQYDVQQDEw5CcmlhbiBDYW1
           wYmVsbDAeFw0xMzAyMjEyMzI5MTVaFw0xODA4MTQyMjI5MTVaMGIxCzAJBg
           NVBAYTAlVTMQswCQYDVQQIEwJDTzEPMA0GA1UEBxMGRGVudmVyMRwwGgYDV
           QQKExNQaW5nIElkZW50aXR5IENvcnAuMRcwFQYDVQQDEw5CcmlhbiBDYW1w
           YmVsbDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL64zn8/QnH
           YMeZ0LncoXaEde1fiLm1jHjmQsF/449IYALM9if6amFtPDy2yvz3YlRij66
           s5gyLCyO7ANuVRJx1NbgizcAblIgjtdf/u3WG7K+IiZhtELto/A7Fck9Ws6
           SQvzRvOE8uSirYbgmj6He4iO8NCyvaK0jIQRMMGQwsU1quGmFgHIXPLfnpn
           fajr1rVTAwtgV5LEZ4Iel+W1GC8ugMhyr4/p1MtcIM42EA8BzE6ZQqC7VPq
           PvEjZ2dbZkaBhPbiZAS3YeYBRDWm1p1OZtWamT3cEvqqPpnjL1XyW+oyVVk
           aZdklLQp2Btgt9qr21m42f4wTw+Xrp6rCKNb0CAwEAATANBgkqhkiG9w0BA
           QUFAAOCAQEAh8zGlfSlcI0o3rYDPBB07aXNswb4ECNIKG0CETTUxmXl9KUL
           +9gGlqCz5iWLOgWsnrcKcY0vXPG9J1r9AqBNTqNgHq2G03X09266X5CpOe1
           zFo+Owb1zxtp3PehFdfQJ610CDLEaS9V9Rqp17hCyybEpOGVwe8fnk+fbEL
           2Bo3UPGrpsHzUoaGpDftmWssZkhpBJKVMJyf/RuP2SmmaIzmnw9JiSlYhzo
           4tpzd5rFXhjRbg4zW9C+2qok+2+qDM1iJ684gPHMIY8aLWrdgQTxkumGmTq
           gawR+N5MDtdPTEQ0XfIBc2cJEUyMTY5MPvACWpkA6SdS4xSvdXK3IVfOWA=="]})JWK",
        };
        for (auto& s : raw) s = stripWhitespace(s);
        return raw;
    }();
    return v;
}

// SHA-256 thumbprints of the above keys, hex-encoded.
const char* const cookbookJWKThumbprints[] = {
    "747ae2dd2003664aeeb21e4753fe7402846170a16bc8df8f23a8cf06d3cbe793",
    "90facafea9b1556698540f70c0117a22ea37bd5cf3ed3c47093c1707282b4b89",
    "747ae2dd2003664aeeb21e4753fe7402846170a16bc8df8f23a8cf06d3cbe793",
    "f63838e96077ad1fc01c3f8405774dedc0641f558ebb4b40dccf5f9b6d66a932",
    "0fc478f8579325fcee0d4cbc6d9d1ce21730a6e97e435d6008fb379b0ebe47d4",
    "0ddb05bfedbec2070fa037324ba397396561d3425d6d69245570c261dc49dee3",
};

}  // namespace

TEST(GoJWK, CurveSize) {
    EXPECT_EQ(curveSize(*elliptic::P256()), 32);
    EXPECT_EQ(curveSize(*elliptic::P384()), 48);
    EXPECT_EQ(curveSize(*elliptic::P521()), 66);
}

TEST(GoJWK, RoundtripRsaPrivate) {
    const auto& k = keys();
    auto [jwk, err] = fromRsaPrivateKey(*k.rsa);
    ASSERT_FALSE(err);
    auto [rsa2, err2] = jwk.rsaPrivateKey();
    ASSERT_FALSE(err2) << err2.Error();
    EXPECT_EQ(rsa2->PublicKey.N.Cmp(k.rsa->PublicKey.N), 0);
    EXPECT_EQ(rsa2->PublicKey.E, k.rsa->PublicKey.E);
    EXPECT_EQ(rsa2->D.Cmp(k.rsa->D), 0);
    ASSERT_EQ(rsa2->Primes.size(), 2u);
    EXPECT_EQ(rsa2->Primes[0].Cmp(k.rsa->Primes[0]), 0);
    EXPECT_EQ(rsa2->Primes[1].Cmp(k.rsa->Primes[1]), 0);
}

TEST(GoJWK, RsaPrivateInsufficientPrimes) {
    rsa::PrivateKey broken;
    broken.PublicKey.N = keys().rsa->PublicKey.N;
    broken.PublicKey.E = keys().rsa->PublicKey.E;
    broken.D = keys().rsa->D;
    broken.Primes = {keys().rsa->Primes[0]};
    auto [raw, err] = fromRsaPrivateKey(broken);
    EXPECT_EQ(err, jose::ErrUnsupportedKeyType);
}

TEST(GoJWK, RsaPrivateExcessPrimes) {
    rsa::PrivateKey broken;
    broken.PublicKey.N = keys().rsa->PublicKey.N;
    broken.PublicKey.E = keys().rsa->PublicKey.E;
    broken.D = keys().rsa->D;
    broken.Primes = {keys().rsa->Primes[0], keys().rsa->Primes[1], *big::NewInt(3)};
    auto [raw, err] = fromRsaPrivateKey(broken);
    EXPECT_EQ(err, jose::ErrUnsupportedKeyType);
}

TEST(GoJWK, RsaKeyChecks) {
    struct Case {
        const char* name;
        bool pub;                                // parse as a public key (n and e only)
        int limit;                               // override for MaxRSAModulusBits, 0 for default
        std::function<void(rawJSONWebKey&)> mutate;  // applied to a valid JWK of the test key
        const char* wantErr;                     // substring of the expected error, empty for success
    };
    int bits = keys().rsa->PublicKey.N.BitLen();
    std::vector<Case> cases = {
        {"public key, modulus one under limit", true, 0,
         [](auto& j) { j.N = newBuffer(oddOfBits(jose::MaxRSAModulusBits - 1)); }, ""},
        {"public key, modulus at limit", true, 0,
         [](auto& j) { j.N = newBuffer(oddOfBits(jose::MaxRSAModulusBits)); }, ""},
        {"public key, modulus over limit", true, 0,
         [](auto& j) { j.N = newBuffer(oddOfBits(jose::MaxRSAModulusBits + 1)); }, "modulus too large"},
        {"private key, modulus over limit", false, 0,
         [](auto& j) { j.N = newBuffer(oddOfBits(jose::MaxRSAModulusBits + 1)); }, "modulus too large"},
        {"public key, limit lowered below key", true, bits - 1, nullptr, "modulus too large"},
        {"private key, limit lowered below key", false, bits - 1, nullptr, "modulus too large"},
        {"public key, limit raised to key", true, bits, nullptr, ""},
        {"private key, limit raised to key", false, bits, nullptr, ""},
        {"p nudged", false, 0,
         [](auto& j) {
             auto p = slice<byte>::of(j.P->bytes());
             p[p.size() - 1] ^= 2;
             j.P = newBuffer(p);
         },
         "p * q != n"},
        {"p = q", false, 0, [](auto& j) { j.Q = j.P; }, "p * q != n"},
        {"p = 1", false, 0, [](auto& j) { j.P = newBufferFromInt(1); }, "p * q != n"},
        {"p = 0", false, 0, [](auto& j) { j.P = newBuffer({}); }, "p * q != n"},
        {"enormous p", false, 0, [](auto& j) { j.P = newBuffer(oddOfBits(1 << 20)); }, "p * q != n"},
    };
    for (auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        ModulusLimit restore;
        jose::MaxRSAModulusBits = 4096;
        auto [jwk, err] = fromRsaPrivateKey(*keys().rsa);
        ASSERT_FALSE(err);
        if (tc.mutate) tc.mutate(jwk);
        if (tc.limit) jose::MaxRSAModulusBits = tc.limit;
        error got;
        if (tc.pub) {
            rawJSONWebKey pub;
            pub.N = jwk.N;
            pub.E = jwk.E;
            got = pub.rsaPublicKey().second;
        } else {
            got = jwk.rsaPrivateKey().second;
        }
        if (std::string_view(tc.wantErr).empty()) {
            EXPECT_FALSE(got) << got.Error();
        } else {
            EXPECT_TRUE(contains(got, tc.wantErr)) << got.Error();
        }
    }
}

TEST(GoJWK, RoundtripEcPublic) {
    for (auto& k : {keys().ec256, keys().ec384, keys().ec521}) {
        auto [jwk, err] = fromEcPublicKey(k->PublicKey);
        ASSERT_FALSE(err);
        auto [ec2, err2] = jwk.ecPublicKey();
        ASSERT_FALSE(err2) << err2.Error();
        EXPECT_EQ(ec2->Curve.get(), k->PublicKey.Curve.get());
        EXPECT_EQ(ec2->X.Cmp(k->PublicKey.X), 0);
        EXPECT_EQ(ec2->Y.Cmp(k->PublicKey.Y), 0);
    }
}

TEST(GoJWK, RoundtripEcPrivate) {
    for (auto& k : {keys().ec256, keys().ec384, keys().ec521}) {
        auto [jwk, err] = fromEcPrivateKey(*k);
        ASSERT_FALSE(err);
        auto [ec2, err2] = jwk.ecPrivateKey();
        ASSERT_FALSE(err2) << err2.Error();
        EXPECT_EQ(ec2->PublicKey.Curve.get(), k->PublicKey.Curve.get());
        EXPECT_EQ(ec2->PublicKey.X.Cmp(k->PublicKey.X), 0);
        EXPECT_EQ(ec2->PublicKey.Y.Cmp(k->PublicKey.Y), 0);
        EXPECT_EQ(ec2->D.Cmp(k->D), 0);
    }
}

TEST(GoJWK, MarshalUnmarshal) {
    const auto& k = keys();
    std::vector<ptr<object>> all = {k.ec256, k.ec256->Public(), k.ec384, k.ec384->Public(), k.ec521,
                                    k.ec521->Public(), k.rsa, k.rsa->Public(), k.edPriv, k.edPriv->Public()};
    int i = 0;
    for (auto& key : all) {
        for (std::string_view use : {"", "sig", "enc"}) {
            SCOPED_TRACE(i);
            jose::JSONWebKey jwk;
            jwk.Key = key;
            jwk.KeyID = "DEADBEEF";
            jwk.Algorithm = "foo";
            jwk.Use = use;

            auto [jsonbar, err] = jwk.MarshalJSON();
            ASSERT_FALSE(err) << err.Error();

            jose::JSONWebKey jwk2;
            error err2 = jwk2.UnmarshalJSON(jsonbar);
            ASSERT_FALSE(err2) << err2.Error() << " " << str(jsonbar);

            auto [jsonbar2, err3] = jwk2.MarshalJSON();
            ASSERT_FALSE(err3);
            EXPECT_EQ(str(jsonbar), str(jsonbar2)) << "roundtrip should not lose information";
            EXPECT_EQ(jwk2.KeyID, "DEADBEEF");
            EXPECT_EQ(jwk2.Algorithm, "foo");
            EXPECT_EQ(jwk2.Use, use);
        }
        i++;
    }
}

TEST(GoJWK, MarshalNonPointer) {
    struct EmbedsKey {
        jose::JSONWebKey Key;
    };
    std::string_view keyJSON = R"({
		"e": "AQAB",
		"kty": "RSA",
		"n": "vd7rZIoTLEe-z1_8G1FcXSw9CQFEJgV4g9V277sER7yx5Qjz_Pkf2YVth6wwwFJEmzc0hoKY-MMYFNwBE4hQHw"
	})";
    jose::JSONWebKey parsed;
    ASSERT_FALSE(json::Unmarshal(keyJSON, &parsed));
    EmbedsKey ek{parsed};
    auto [out, err] = json::Marshal(ek);
    ASSERT_FALSE(err);
    EXPECT_EQ(str(out),
              "{\"Key\":{\"kty\":\"RSA\",\"n\":\"vd7rZIoTLEe-z1_8G1FcXSw9CQFEJgV4g9V277sER7yx5Qjz_Pkf2YVth6wwwFJEmzc0hoKY-"
              "MMYFNwBE4hQHw\",\"e\":\"AQAB\"}}");
}

TEST(GoJWK, MarshalUnmarshalInvalid) {
    const auto& k = keys();
    // A coordinate one byte too large, and not zero.
    auto invalidCoord = slice<byte>::make(curveSize(*elliptic::P256()) + 1);
    invalidCoord[0] = 1;
    big::Int tooLarge;
    tooLarge.SetBytes(invalidCoord);

    std::vector<ptr<object>> bad;
    // Empty keys
    bad.push_back(make_ptr<rsa::PrivateKey>());
    bad.push_back(make_ptr<ecdsa::PrivateKey>());
    // Missing values in the public key
    auto p1 = make_ptr<ecdsa::PrivateKey>();
    p1->PublicKey.Curve = elliptic::P256();
    bad.push_back(p1);
    // No curve
    auto p2 = make_ptr<ecdsa::PrivateKey>();
    p2->PublicKey.X = k.ec256->PublicKey.X;
    p2->PublicKey.Y = k.ec256->PublicKey.Y;
    bad.push_back(p2);
    // A valid public key, but missing private key values
    auto p3 = make_ptr<ecdsa::PrivateKey>();
    p3->PublicKey = k.ec256->PublicKey;
    bad.push_back(p3);
    // An invalid public key: values too large
    auto p4 = make_ptr<ecdsa::PrivateKey>();
    p4->PublicKey.Curve = k.ec256->PublicKey.Curve;
    p4->PublicKey.X = tooLarge;
    p4->PublicKey.Y = tooLarge;
    p4->D = k.ec256->D;
    bad.push_back(p4);
    // No key
    bad.push_back(nullptr);

    int i = 0;
    for (auto& key : bad) {
        jose::JSONWebKey jwk;
        jwk.Key = key;
        EXPECT_TRUE(jwk.MarshalJSON().second) << "managed to serialize invalid key " << i;
        i++;
    }
}

TEST(GoJWK, WebKeyVectorsInvalid) {
    for (std::string_view key : {
             // Invalid JSON
             "{X",
             // Empty key
             "{}",
             // Invalid RSA keys
             R"({"kty":"RSA"})",
             R"({"kty":"RSA","e":""})",
             R"({"kty":"RSA","e":"XXXX"})",
             R"({"kty":"RSA","d":"XXXX"})",
             // Invalid EC keys
             R"({"kty":"EC","crv":"ABC"})",
             R"({"kty":"EC","crv":"P-256"})",
             R"({"kty":"EC","crv":"P-256","d":"XXX"})",
             R"({"kty":"EC","crv":"ABC","d":"dGVzdA","x":"dGVzdA"})",
             R"({"kty":"EC","crv":"P-256","d":"dGVzdA","x":"dGVzdA"})",
         }) {
        EXPECT_TRUE(unmarshal(key).second) << "managed to parse invalid key: " << key;
    }
}

// TestJWKUnsupported checks for an error when parsing a JWK with an unsupported key type.
TEST(GoJWK, JWKUnsupported) {
    auto [jwk, err] = unmarshal(R"({"kty": "XXX"})");
    EXPECT_TRUE(errors::Is(err, jose::ErrUnsupportedKeyType)) << err.Error();
    // Symmetric keys are not ported.
    err = unmarshal(R"({"kty":"oct","alg":"A128KW","k":"GawgguFyGrWKav7AX4VKUg"})").second;
    EXPECT_TRUE(errors::Is(err, jose::ErrUnsupportedKeyType)) << err.Error();
    // An OKP key of another curve.
    err = unmarshal(R"({"kty":"OKP","crv":"X25519","x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"})").second;
    EXPECT_TRUE(errors::Is(err, jose::ErrUnsupportedKeyType)) << err.Error();
    // No kty at all is another error.
    err = unmarshal(R"({"kid":"a"})").second;
    EXPECT_TRUE(contains(err, "missing json web key type")) << err.Error();
    EXPECT_FALSE(errors::Is(err, jose::ErrUnsupportedKeyType));
}

TEST(GoJWK, WebKeyVectorsValid) {
    for (auto& key : cookbookJWKs()) {
        auto [jwk, err] = unmarshal(key);
        EXPECT_FALSE(err) << "unable to parse valid key: " << key << err.Error();
        EXPECT_TRUE(jwk.Valid());
    }
}

TEST(GoJWK, Ed25519Serialization) {
    jose::JSONWebKey jwk;
    jwk.Key = keys().edPriv;
    auto [serialized, err] = json::Marshal(jwk);
    ASSERT_FALSE(err);
    jose::JSONWebKey jwk2;
    ASSERT_FALSE(json::Unmarshal(serialized, &jwk2));
    auto priv2 = gopp::dynamic_pointer_cast<ed25519::PrivateKey>(jwk2.Key);
    ASSERT_TRUE(priv2);
    EXPECT_TRUE(keys().edPriv->Public()->Equal(*priv2->Public()));
}

TEST(GoJWK, Ed25519RejectsMalformedPublicJWK) {
    slice<byte> overlongX = appendSpan(slice<byte>::of(keys().edPub->key), std::span<const byte>(std::array<byte, 1>{0x42}));
    struct Case {
        const char* name;
        slice<byte> x;
    };
    std::vector<Case> cases = {
        {"short x", slice<byte>{0x01}},
        {"overlong x", overlongX},
        {"empty x", slice<byte>::make(0)},
        {"identity point", fromHex("01000000000000000000000000000000 00000000000000000000000000000000")},
        {"identity point sign-bit alias", fromHex("01000000000000000000000000000000 00000000000000000000000000000080")},
        {"zero point", fromHex("00000000000000000000000000000000 00000000000000000000000000000000")},
        {"low-order point", fromHex("26e8958fc2b227b045c3f489f2ef98f0 d5dfac05d3c63339b13802886d53fc05")},
        {"low-order point sign-bit variant", fromHex("26e8958fc2b227b045c3f489f2ef98f0 d5dfac05d3c63339b13802886d53fc85")},
        {"non-canonical low-order point", fromHex("edffffffffffffffffffffffffffffff ffffffffffffffffffffffffffffff7f")},
        {"non-canonical low-order sign-bit variant",
         fromHex("edffffffffffffffffffffffffffffff ffffffffffffffffffffffffffffffff")},
    };
    for (auto& tc : cases) {
        std::string raw = rawEd25519JWK(tc.x, nullptr);
        EXPECT_TRUE(unmarshal(raw).second) << tc.name << ": accepted malformed Ed25519 public JWK " << raw;
        // Not through the set either: the error is not "unsupported".
        jose::JSONWebKeySet set;
        EXPECT_TRUE(set.UnmarshalJSON(as_bytes(R"({"keys":[)" + raw + "]}"))) << tc.name;
        // A key that is not 32 bytes never reaches ed25519::Verify.
        EXPECT_FALSE(unmarshal(raw).first.Key) << tc.name;
    }
}

TEST(GoJWK, Ed25519RejectsMalformedPrivateJWK) {
    auto other = std::get<1>(ed25519::GenerateKey(nullptr));
    auto seed = keys().edPriv->Seed();
    struct Case {
        const char* name;
        slice<byte> x, d;
    };
    std::vector<Case> cases = {
        {"short d", keys().edPub->key, seed.Sub(0, seed.size() - 1)},
        {"short x", slice<byte>{0x01}, seed},
        {"mismatched x", other->key.Sub(32), seed},
    };
    for (auto& tc : cases) {
        std::string raw = rawEd25519JWK(tc.x, &tc.d);
        EXPECT_TRUE(unmarshal(raw).second) << tc.name << ": accepted malformed Ed25519 private JWK " << raw;
    }
}

TEST(GoJWK, Ed25519PublicAndPrivateAccepted) {
    std::string raw = rawEd25519JWK(keys().edPub->key, nullptr);
    auto [jwk, err] = unmarshal(raw);
    ASSERT_FALSE(err) << err.Error();
    auto pub = gopp::dynamic_pointer_cast<ed25519::PublicKey>(jwk.Key);
    ASSERT_TRUE(pub);
    EXPECT_EQ(pub->key.size(), 32u);
    EXPECT_TRUE(pub->Equal(*keys().edPub));
    EXPECT_TRUE(jwk.IsPublic());
    // The key is a copy of x, so that it can be verified with.
    auto sig = ed25519::Sign(*keys().edPriv, as_bytes("hello"));
    EXPECT_TRUE(ed25519::Verify(*pub, as_bytes("hello"), sig));
}

TEST(GoJWK, Thumbprint) {
    const auto& all = cookbookJWKs();
    for (size_t i = 0; i < all.size(); i++) {
        auto [jwk, err] = unmarshal(all[i]);
        ASSERT_FALSE(err) << all[i];
        auto [tp, err2] = jwk.Thumbprint(crypto::Hash::SHA256);
        ASSERT_FALSE(err2) << err2.Error();
        EXPECT_EQ(std::string(hex::EncodeToString(tp)), cookbookJWKThumbprints[i]) << i;
        // The thumbprint of the public key is the same.
        EXPECT_EQ(std::string(hex::EncodeToString(jwk.Public().Thumbprint(crypto::Hash::SHA256).first)),
                  cookbookJWKThumbprints[i])
            << i;
    }
}

TEST(GoJWK, MarshalUnmarshalJWKSet) {
    jose::JSONWebKey jwk1, jwk2;
    jwk1.Key = jwk2.Key = keys().rsa;
    jwk1.KeyID = "ABCDEFG";
    jwk2.KeyID = "GFEDCBA";
    jwk1.Algorithm = jwk2.Algorithm = "foo";
    jose::JSONWebKeySet set;
    set.Keys = append(set.Keys, jwk1);
    set.Keys = append(set.Keys, jwk2);

    auto [jsonbar, err] = json::Marshal(set);
    ASSERT_FALSE(err);
    jose::JSONWebKeySet set2;
    ASSERT_FALSE(json::Unmarshal(jsonbar, &set2));
    ASSERT_EQ(set2.Keys.size(), 2u);
    auto [jsonbar2, err2] = json::Marshal(set2);
    ASSERT_FALSE(err2);
    EXPECT_EQ(str(jsonbar), str(jsonbar2)) << "roundtrip should not lose information";
    EXPECT_EQ(str(jsonbar).substr(0, 9), "{\"keys\":[");
}

TEST(GoJWK, JWKSetKey) {
    jose::JSONWebKey jwk1, jwk2;
    jwk1.Key = jwk2.Key = keys().rsa;
    jwk1.KeyID = "ABCDEFG";
    jwk2.KeyID = "GFEDCBA";
    jose::JSONWebKeySet set;
    set.Keys = append(set.Keys, jwk1);
    set.Keys = append(set.Keys, jwk2);
    auto k = set.Key("ABCDEFG");
    ASSERT_EQ(k.size(), 1u);
    EXPECT_EQ(k[0].KeyID, "ABCDEFG");
    EXPECT_EQ(set.Key("nope").size(), 0u);
}

TEST(GoJWK, UnmarshalPrivateJWKAllowed) {
    jose::JSONWebKey in;
    in.Key = keys().rsa;
    auto [raw, err] = json::Marshal(in);
    ASSERT_FALSE(err);

    jose::JSONWebKey jwk;
    ASSERT_FALSE(json::Unmarshal(raw, &jwk));
    EXPECT_TRUE(gopp::dynamic_pointer_cast<rsa::PrivateKey>(jwk.Key));

    jose::JSONWebKeySet jwks;
    ASSERT_FALSE(json::Unmarshal(as_bytes("{\"keys\":[" + str(raw) + "]}"), &jwks));
    ASSERT_EQ(jwks.Keys.size(), 1u);
    EXPECT_TRUE(gopp::dynamic_pointer_cast<rsa::PrivateKey>(jwks.Keys[0].Key));
}

TEST(GoJWK, JWKIsPublic) {
    auto eccPub = make_ptr<ecdsa::PublicKey>();
    eccPub->Curve = elliptic::P256();
    auto rsaPub = make_ptr<rsa::PublicKey>();
    rsaPub->E = 1;
    auto eccPriv = make_ptr<ecdsa::PrivateKey>();
    eccPriv->PublicKey.Curve = elliptic::P256();
    auto rsaPriv = make_ptr<rsa::PrivateKey>();
    rsaPriv->PublicKey.E = 1;
    rsaPriv->Primes = {big::Int(), big::Int()};

    struct Case {
        ptr<object> key;
        bool expectedIsPublic;
    };
    for (auto& tc : std::vector<Case>{{eccPub, true},
                                      {eccPriv, false},
                                      {rsaPub, true},
                                      {rsaPriv, false},
                                      {keys().edPub, true},
                                      {keys().edPriv, false},
                                      {nullptr, false}}) {
        jose::JSONWebKey k;
        k.Key = tc.key;
        EXPECT_EQ(k.IsPublic(), tc.expectedIsPublic);
    }
}

TEST(GoJWK, JWKValid) {
    auto eccPub = make_ptr<ecdsa::PublicKey>();
    eccPub->Curve = elliptic::P256();
    auto rsaPub = make_ptr<rsa::PublicKey>();
    rsaPub->E = 1;
    auto eccPriv = make_ptr<ecdsa::PrivateKey>();
    eccPriv->PublicKey = *eccPub;
    auto rsaPriv = make_ptr<rsa::PrivateKey>();
    rsaPriv->PublicKey = *rsaPub;
    rsaPriv->Primes = {big::Int(), big::Int()};
    auto edPubEmpty = make_ptr<ed25519::PublicKey>();
    auto edPrivEmpty = make_ptr<ed25519::PrivateKey>();

    struct Case {
        ptr<object> key;
        bool expectedValidity;
    };
    for (auto& tc : std::vector<Case>{{nullptr, false},
                                      {make_ptr<ecdsa::PublicKey>(), false},
                                      {eccPub, true},
                                      {make_ptr<ecdsa::PrivateKey>(), false},
                                      {eccPriv, true},
                                      {make_ptr<rsa::PublicKey>(), false},
                                      {rsaPub, true},
                                      {make_ptr<rsa::PrivateKey>(), false},
                                      {rsaPriv, true},
                                      {keys().edPub, true},
                                      {keys().edPriv, true},
                                      {edPubEmpty, false},
                                      {edPrivEmpty, false}}) {
        jose::JSONWebKey k;
        k.Key = tc.key;
        bool valid = k.Valid();
        EXPECT_EQ(valid, tc.expectedValidity);
        if (valid) {
            bool wasPublic = k.IsPublic();
            jose::JSONWebKey p = k.Public();  // all aforementioned keys are asymmetric
            EXPECT_TRUE(p.Valid()) << "unable to derive public key from valid asymmetric key";
            EXPECT_EQ(wasPublic, k.IsPublic()) << "original key was touched during public key derivation";
            EXPECT_TRUE(p.IsPublic());
        }
    }
}

TEST(GoJWK, JWKBufferSizeCheck) {
    auto [jwk, err] = unmarshal(R"({
		"kty":"EC",
		"crv":"P-256",
		"x":"m9GSmJ5iGmAYlMlaOJGSFN_CjN9cIn8GGYExP-C0FBiIXlWTNvGN38R9WdrHcppfsKF0FXMOMyutpHIRaiMxYSA",
		"y":"ZaPcRZ3q_7T3h-Gwz2i-T2JjJXfj6YVGgKHcFz5zqmg"})");
    ASSERT_TRUE(err) << "key should be invalid";
    EXPECT_FALSE(jwk.Valid());
    // Go panicked here: newFixedSizeBuffer (len(data) > length).
    EXPECT_TRUE(jwk.Thumbprint(crypto::Hash::SHA256).second);

    // A key made by hand with coordinates that are too large.
    auto ec = make_ptr<ecdsa::PublicKey>();
    ec->Curve = elliptic::P256();
    ec->X.SetBytes(oddOfBits(264));
    ec->Y.SetBytes(oddOfBits(264));
    jose::JSONWebKey big;
    big.Key = ec;
    EXPECT_TRUE(big.Thumbprint(crypto::Hash::SHA256).second);
}

TEST(GoJWK, JWKPaddingPrivateX) {
    auto [jwk, err] = unmarshal(R"({
    "kty": "EC",
    "crv": "P-256",
    "x": "nPTIABcDASY6FNGSNfHCB51tY7qChtgzeVazOtLrwQ",
    "y": "vEEs4V0egJkNyM2Q4pp001zu14VcpQ0_Ei8xOOPxKZs",
    "d": "nIVCvMR2wkLmeGJErOpI23VDHl2s3JwGdbzKy0odir0"
  })");
    ASSERT_TRUE(err) << "Expected key with short x to fail unmarshalling";
    EXPECT_TRUE(contains(err, "wrong length for x")) << err.Error();
    EXPECT_FALSE(jwk.Valid());
}

TEST(GoJWK, JWKPaddingPrivateY) {
    auto [jwk, err] = unmarshal(R"({
    "kty": "EC",
    "crv": "P-256",
    "x": "vEEs4V0egJkNyM2Q4pp001zu14VcpQ0_Ei8xOOPxKZs",
    "y": "nPTIABcDASY6FNGSNfHCB51tY7qChtgzeVazOtLrwQ",
    "d": "nIVCvMR2wkLmeGJErOpI23VDHl2s3JwGdbzKy0odir0"
  })");
    ASSERT_TRUE(err) << "Expected key with short y to fail unmarshalling";
    EXPECT_TRUE(contains(err, "wrong length for y")) << err.Error();
    EXPECT_FALSE(jwk.Valid());
}

TEST(GoJWK, JWKPaddingPrivateD) {
    auto [jwk, err] = unmarshal(R"({
    "kty": "EC",
    "crv": "P-256",
    "x": "vEEs4V0egJkNyM2Q4pp001zu14VcpQ0_Ei8xOOPxKZs",
    "y": "qnPTIABcDASY6FNGSNfHCB51tY7qChtgzeVazOtLrwQ",
    "d": "IVCvMR2wkLmeGJErOpI23VDHl2s3JwGdbzKy0odir0"
  })");
    ASSERT_TRUE(err) << "Expected key with short d to fail unmarshalling";
    EXPECT_TRUE(contains(err, "wrong length for d")) << err.Error();
    EXPECT_FALSE(jwk.Valid());
}

TEST(GoJWK, JWKPaddingX) {
    auto [jwk, err] = unmarshal(R"({
    "kty": "EC",
    "crv": "P-256",
    "x": "nPTIABcDASY6FNGSNfHCB51tY7qChtgzeVazOtLrwQ",
    "y": "vEEs4V0egJkNyM2Q4pp001zu14VcpQ0_Ei8xOOPxKZs"
  })");
    ASSERT_TRUE(err) << "Expected key with short x to fail unmarshalling";
    EXPECT_TRUE(contains(err, "wrong length for x")) << err.Error();
    EXPECT_FALSE(jwk.Valid());
}

TEST(GoJWK, JWKPaddingY) {
    auto [jwk, err] = unmarshal(R"({
    "kty": "EC",
    "crv": "P-256",
    "x": "vEEs4V0egJkNyM2Q4pp001zu14VcpQ0_Ei8xOOPxKZs",
    "y": "nPTIABcDASY6FNGSNfHCB51tY7qChtgzeVazOtLrwQ"
  })");
    ASSERT_TRUE(err) << "Expected key with short y to fail unmarshalling";
    EXPECT_TRUE(contains(err, "wrong length for y")) << err.Error();
    EXPECT_FALSE(jwk.Valid());
}

// ---------------------------------------------------------------------------
// What JWKS from a remote server must not get through.

TEST(GoJWK, UntrustedEcPointNotOnCurve) {
    // The coordinates of the test key, with y changed.
    const auto& pub = keys().ec256->PublicKey;
    auto x = newFixedSizeBuffer(pub.X.Bytes(), 32);
    auto y = newFixedSizeBuffer(pub.Y.Bytes(), 32);
    auto y2 = slice<byte>::of(y.bytes());
    y2[31] ^= 1;
    std::string raw = R"({"kty":"EC","crv":"P-256","x":")" + std::string(x.base64()) + R"(","y":")" +
                      std::string(newBuffer(y2).base64()) + "\"}";
    auto [jwk, err] = unmarshal(raw);
    EXPECT_TRUE(contains(err, "not on declared curve")) << err.Error();
    EXPECT_FALSE(jwk.Key);
    // And as a private key.
    raw.pop_back();
    raw += R"(,"d":")" + std::string(newFixedSizeBuffer(keys().ec256->D.Bytes(), 32).base64()) + "\"}";
    err = unmarshal(raw).second;
    EXPECT_TRUE(contains(err, "not on declared curve")) << err.Error();
    // The right point is accepted, and is on the curve of all three sizes.
    for (auto& k : {keys().ec256, keys().ec384, keys().ec521}) {
        jose::JSONWebKey jwk2;
        jwk2.Key = k->Public();
        auto [js, err2] = jwk2.MarshalJSON();
        ASSERT_FALSE(err2);
        EXPECT_FALSE(unmarshal(str(js)).second);
    }
}

TEST(GoJWK, UntrustedEcCoordinatesOutOfField) {
    // x = y = p, of the right length, is not a point.
    auto p = elliptic::P256()->Params().P.Bytes();
    std::string raw = R"({"kty":"EC","crv":"P-256","x":")" + std::string(newBuffer(p).base64()) + R"(","y":")" +
                      std::string(newBuffer(p).base64()) + "\"}";
    EXPECT_TRUE(unmarshal(raw).second);
    // The point at (0, 0) is not on P-256.
    auto zero = slice<byte>::make(32);
    raw = R"({"kty":"EC","crv":"P-256","x":")" + std::string(newBuffer(zero).base64()) + R"(","y":")" +
          std::string(newBuffer(zero).base64()) + "\"}";
    EXPECT_TRUE(unmarshal(raw).second);
}

TEST(GoJWK, UntrustedRsaPublic) {
    for (std::string_view raw : {
             R"({"kty":"RSA","n":"AQAB"})",                       // no e
             R"({"kty":"RSA","e":"AQAB"})",                       // no n
             R"({"kty":"RSA","n":"AQAB","e":"AA"})",              // e = 0
             R"({"kty":"RSA","n":"AQAB","e":"AQ"})",              // e = 1
             R"({"kty":"RSA","n":"AQAB","e":"AQAAAAAAAAAAAA"})",  // e does not fit an int64
             R"({"kty":"RSA","n":"AQAB","e":"_____________w"})",  // e does not fit an int64
             R"({"kty":"RSA","n":"AQAB","e":"_w"})" "x",          // not JSON
             R"({"kty":"RSA","n":1,"e":"AQAB"})",                 // n is not a string
             R"({"kty":"RSA","n":"!!!","e":"AQAB"})",             // not base64url
         }) {
        EXPECT_TRUE(unmarshal(raw).second) << raw;
    }
    // A small public key is parsed.
    auto [jwk, err] = unmarshal(R"({"kty":"RSA","n":"AQAB","e":"AQAB"})");
    EXPECT_FALSE(err);
    auto pub = gopp::dynamic_pointer_cast<rsa::PublicKey>(jwk.Key);
    ASSERT_TRUE(pub);
    EXPECT_EQ(pub->E, 65537);
}

TEST(GoJWK, ByteBuffer) {
    EXPECT_EQ(std::string(newBufferFromInt(65537).base64()), "AQAB");
    EXPECT_EQ(std::string(newBufferFromInt(0).base64()), "");
    EXPECT_EQ(newFixedSizeBuffer(as_bytes("a"), 3).bytes().size(), 3u);
    EXPECT_EQ(stripWhitespace(" a\n b\t\r c "), "abc");
}

TEST(GoJWK, X509MembersIgnored) {
    // Real JWKS carry x5c, x5t and x5u next to the key; they aren't used.
    auto [jwk, err] = unmarshal(
        R"({"kty":"RSA","kid":"k","use":"sig","alg":"RS256","n":"AQAB","e":"AQAB","x5c":["MIIB"],"x5t":"!!","x5t#S256":"x","x5u":"::"})");
    EXPECT_FALSE(err) << err.Error();
    EXPECT_EQ(jwk.KeyID, "k");
    EXPECT_EQ(jwk.Use, "sig");
    EXPECT_EQ(jwk.Algorithm, "RS256");
}

TEST(GoJWK, KeySetSkipsUnsupported) {
    jose::JSONWebKey a, b;
    a.Key = keys().rsa->Public();
    a.KeyID = "a";
    b.Key = keys().edPub;
    b.KeyID = "b";
    std::string ja = str(a.MarshalJSON().first), jb = str(b.MarshalJSON().first);
    jose::JSONWebKeySet set;
    error err = set.UnmarshalJSON(as_bytes(R"({"keys":[)" + ja + R"(,{"kty":"oct","k":"AA","kid":"o"},{"kty":"XXX"},)" +
                                           jb + R"(,{"kty":"OKP","crv":"X25519","x":"AA"}]})"));
    ASSERT_FALSE(err) << err.Error();
    ASSERT_EQ(set.Keys.size(), 2u);
    EXPECT_EQ(set.Keys[0].KeyID, "a");
    EXPECT_EQ(set.Keys[1].KeyID, "b");
    EXPECT_EQ(set.Key("o").size(), 0u);
    // The set through json::Unmarshal.
    jose::JSONWebKeySet set2;
    EXPECT_FALSE(json::Unmarshal(std::string_view(R"({"keys":[{"kty":"oct","k":"AA"}]})"), &set2));
    EXPECT_EQ(set2.Keys.size(), 0u);
}

TEST(GoJWK, KeySetErrors) {
    jose::JSONWebKeySet set;
    // A malformed key fails the set, and leaves it as it was.
    set.Keys = append(set.Keys, jose::JSONWebKey{});
    EXPECT_TRUE(set.UnmarshalJSON(as_bytes(R"({"keys":[{"kty":"RSA"}]})")));
    EXPECT_EQ(set.Keys.size(), 1u);
    EXPECT_TRUE(set.UnmarshalJSON(as_bytes(R"({"keys":)")));
    EXPECT_TRUE(set.UnmarshalJSON(as_bytes(R"({"keys":{}})")));
    EXPECT_TRUE(set.UnmarshalJSON(as_bytes(R"({"keys":[1]})")));
    // No keys is an empty set.
    EXPECT_FALSE(set.UnmarshalJSON(as_bytes(R"({})")));
    EXPECT_EQ(set.Keys.size(), 0u);
    EXPECT_FALSE(set.UnmarshalJSON(as_bytes(R"({"keys":null})")));
    EXPECT_FALSE(set.UnmarshalJSON(as_bytes(R"({"keys":[]})")));
}

TEST(GoJWK, UnmarshalKeepsKeyOnError) {
    jose::JSONWebKey jwk;
    jwk.Key = keys().rsa;
    jwk.KeyID = "keep";
    EXPECT_TRUE(jwk.UnmarshalJSON(as_bytes(R"({"kty":"EC","crv":"P-256"})")));
    EXPECT_EQ(jwk.Key.get(), keys().rsa.get());
    EXPECT_EQ(jwk.KeyID, "keep");
}

TEST(GoJWK, SignatureAlgorithms) {
    EXPECT_EQ(jose::RS256, "RS256");
    EXPECT_EQ(jose::PS512, "PS512");
    EXPECT_EQ(jose::ES384, "ES384");
    EXPECT_EQ(jose::EdDSA, "EdDSA");
    EXPECT_EQ(jose::HS256, "HS256");
    jose::SignatureAlgorithm alg(jose::ES256);
    EXPECT_EQ(alg, "ES256");
}

TEST(GoJWK, MembersAreCaseSensitive) {
    // As go-jose's json: "KTY" is not "kty", and "N" is not "n".
    jose::JSONWebKey good;
    good.Key = keys().rsa->Public();
    good.KeyID = "k";
    std::string j = str(good.MarshalJSON().first);
    ASSERT_FALSE(unmarshal(j).second);

    std::string upper = j;
    upper.replace(upper.find("\"kty\""), 5, "\"KTY\"");
    EXPECT_TRUE(contains(unmarshal(upper).second, "missing json web key type")) << unmarshal(upper).second;
    std::string upperN = j;
    upperN.replace(upperN.find("\"n\""), 3, "\"N\"");
    EXPECT_TRUE(contains(unmarshal(upperN).second, "missing n/e")) << unmarshal(upperN).second;
    // A differently cased duplicate doesn't replace a member.
    auto [jwk, err] = unmarshal(j.substr(0, j.size() - 1) + R"(,"KID":"other","Kid":"x"})");
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(jwk.KeyID, "k");

    // The set: "KEYS" is not "keys".
    jose::JSONWebKeySet set;
    ASSERT_FALSE(json::Unmarshal(std::string_view(R"({"KEYS":[)" + j + "]}"), &set));
    EXPECT_EQ(set.Keys.size(), 0u);
    ASSERT_FALSE(json::Unmarshal(std::string_view(R"({"keys":[)" + j + "]}"), &set));
    EXPECT_EQ(set.Keys.size(), 1u);
}
