// Tests of go++'s crypto, math/big and crypto/elliptic, which crypto/rsa,
// crypto/ecdsa and crypto/ed25519 build on. Go has no such tests of its own:
// crypto's hash identifiers are tested through the packages that register
// them, and the curve parameters through the curves. So these check the
// values Go's crypto.go, params.go and nistec.go give: each Hash's value,
// name and size, and each curve's name, bit size and parameters.
//
// The GoRSA tests are ported from crypto/rsa's pkcs1v15_test.go, pss_test.go
// and rsa_test.go of Go 1.26: the PKCS #1 v1.5 vectors (TestSignPKCS1v15,
// TestVerifyPKCS1v15, TestUnpaddedSignature), TestPSSSigning, TestPSSNilOpts,
// and a key generation test in the spirit of rsa_test.go's. Left out:
// everything about encryption and decryption (OAEP, PKCS #1 v1.5
// encryption: TestDecryptPKCS1v15, TestShortSessionKey, ...), because go++
// doesn't port them; the tests of key serialization, precomputation and the
// FIPS and boring modes; and the golden PSS vectors and PSS513, which test
// Go's own implementation rather than the interface. Go's 512 and 1024 bit
// test keys are given by their parameters, as the x509 parser isn't ported
// yet.
//
// The GoECDSA tests are ported from crypto/ecdsa's ecdsa_test.go of Go 1.26:
// TestSignAndVerify, TestSignAndVerifyASN1, TestNonceSafety, TestINDCCA,
// TestZeroHashSignature, and TestZeroSignature, TestNegativeSignature,
// TestRPlusNSignature and TestRMinusNSignature (as BadScalars), over P-224,
// P-256, P-384 and P-521 (not Go's "generic" P-256 params). Left out:
// TestRandomPoint and the point arithmetic (go++ has none), TestVectors (its
// testdata is the gzipped SigVer.rsp), the ParseUncompressedPublicKey,
// Bytes and ECDH tests (not ported), and the private key validation tests.
// RFC6979Vector replaces TestRFC6979: go++ signs with a random nonce, so it
// verifies the RFC 6979 A.2.5 signature instead of reproducing it. The
// remaining tests (Invalid*, MalformedDER) pin what Go's Verify does with
// input an attacker controls, and Go's tests don't.
//
// The GoEd25519 tests are ported from crypto/ed25519's ed25519_test.go:
// TestSignVerify, TestCryptoSigner, TestEqual, TestMalleability, and
// TestGolden over the first 8 lines of testdata/sign.input.gz (the vectors of
// https://ed25519.cr.yp.to/python/sign.input, whose message is 0 to 7 bytes
// long), written here in hex. Left out: Ed25519ph and Ed25519ctx (Options,
// TestSignVerifyHashed, TestSignVerifyContext), the RFC 8032 vector tests
// through them, TestGenerateKey's nil reader, and TestAllocations.
#include <gopp/crypto.hpp>
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rand.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/hex.hpp>

#include <gtest/gtest.h>

#include "crypto/pkey.hpp"
#include "crypto/rsa_internal.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace gopp;

namespace {

TEST(GoCrypto, HashSizes) {
    struct {
        crypto::Hash h;
        unsigned value;
        isize size;
        const char* name;
    } tests[] = {
        {crypto::Hash::MD5, 2, 16, "MD5"},
        {crypto::Hash::SHA1, 3, 20, "SHA-1"},
        {crypto::Hash::SHA224, 4, 28, "SHA-224"},
        {crypto::Hash::SHA256, 5, 32, "SHA-256"},
        {crypto::Hash::SHA384, 6, 48, "SHA-384"},
        {crypto::Hash::SHA512, 7, 64, "SHA-512"},
    };
    for (auto& t : tests) {
        SCOPED_TRACE(t.name);
        EXPECT_EQ(static_cast<unsigned>(t.h), t.value);
        EXPECT_TRUE(crypto::Available(t.h));
        EXPECT_EQ(crypto::Size(t.h), t.size);
        EXPECT_EQ(crypto::New(t.h)->Size(), t.size);
        EXPECT_EQ(crypto::String(t.h), t.name);
    }
    EXPECT_FALSE(crypto::Available(static_cast<crypto::Hash>(0)));
    EXPECT_FALSE(crypto::Available(static_cast<crypto::Hash>(99)));
    EXPECT_EQ(crypto::String(static_cast<crypto::Hash>(99)), "unknown hash value 99");
}

TEST(GoCrypto, HashNew) {
    // The hash h.New() makes is the one named: the SHA-256 of "abc".
    auto h = crypto::New(crypto::Hash::SHA256);
    h->Write(as_bytes("abc"));
    EXPECT_EQ(std::string(hex::EncodeToString(h->Sum(slice<byte>())).view()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(GoCryptoDeathTest, HashUnknown) {
    EXPECT_DEATH(crypto::New(static_cast<crypto::Hash>(99)), "unavailable");
    EXPECT_DEATH(crypto::Size(static_cast<crypto::Hash>(99)), "Size of unknown hash function");
}

TEST(GoCrypto, CurveParams) {
    struct {
        ptr<elliptic::Curve> c;
        const char* name;
        int bits;
        const char* n;  // the order of the base point, in hex
    } tests[] = {
        {elliptic::P224(), "P-224", 224, "ffffffffffffffffffffffffffff16a2e0b8f03e13dd29455c5c2a3d"},
        {elliptic::P256(), "P-256", 256, "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"},
        {elliptic::P384(), "P-384", 384,
         "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973"},
        {elliptic::P521(), "P-521", 521,
         "1fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899"
         "c47aebb6fb71e91386409"},
    };
    for (auto& t : tests) {
        SCOPED_TRACE(t.name);
        const auto& p = t.c->Params();
        EXPECT_EQ(p.Name, t.name);
        EXPECT_EQ(p.BitSize, t.bits);
        EXPECT_EQ(p.P.BitLen(), t.bits);
        EXPECT_EQ(p.N.Text(16), t.n);

        EXPECT_GT(p.Gx.Sign(), 0);
        EXPECT_GT(p.Gy.Sign(), 0);
        EXPECT_GT(p.B.Sign(), 0);
        EXPECT_LT(p.Gx.Cmp(p.P), 0);
        EXPECT_LT(p.Gy.Cmp(p.P), 0);
        EXPECT_LT(p.B.Cmp(p.P), 0);
        EXPECT_GT(p.N.Sign(), 0);
        // A singleton.
        EXPECT_EQ(t.c.get(), (t.bits == 224   ? elliptic::P224()
                              : t.bits == 256 ? elliptic::P256()
                              : t.bits == 384 ? elliptic::P384()
                                              : elliptic::P521())
                                 .get());
    }
}

TEST(GoCrypto, CurveParamsP256) {
    // Go's p256 constants.
    const auto& p = elliptic::P256()->Params();
    EXPECT_EQ(p.P.Text(16), "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    EXPECT_EQ(p.B.Text(16), "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
    EXPECT_EQ(p.Gx.Text(16), "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    EXPECT_EQ(p.Gy.Text(16), "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
}

// Go's test512Key and test1024Key.
ptr<rsa::PrivateKey> test512Key() {
    return rsa::detail::fromComponents(
        "b2990f49c47dfa8cd400ae6a4d1b8a3b6a13642b23f28b003bfb97790ade9a4cc82b8b2a81747ddec08b6296e53a08c331687ef25c4bf4936ba1c0e6041e9d15",
        65537,
        "8abd6a69f4d1a4b487f0ab8d7aaefd38609405c999984e30f567e1e8aeeff44e8b18bdb1ec78dfa31a55e32a48d7fb131f5af1f44d7d6b2ced2a9df5e5ae4535",
        "dab2f18048baa68de7df04d2d35d5d80e60e2dfa42d50a9b04219032715e46b3",
        "d10f2e66b1d0c13f10ef9927bf5324a379ca218146cbf9cafc795221f16a3117");
}

ptr<rsa::PrivateKey> test1024Key() {
    return rsa::detail::fromComponents(
        "b0d18352a88f53d5516f46c20e7a367d7de88acf54a019f6def57ab9b44ceddb2242b1bca0fb1b5cb82b3036176a63903564dec6eb41db2f8fc787f4e52e1149e33347572973f660c3c77ca9e0821c2b695be7ae9d7d30f4079110f48aae6f8b702d474b2900817f2866249bec12a2b19b8278416808f81ae1fcf9b7778a623f",
        65537,
        "482e9f8fa4e42df30d7581cb42a1bd90e94f7f2b387ecb5aae9643ed7f9f50127f1ffef2e43cde64b182600214f907801d6bfa4df64842345e5bb432d3444525d8301654c5442b0a5e11b9c7e201fa32f41abaf4f0a6e03cf0e0cb8266c62ad11d956d53c9466e48995fea260c8536f041cb3562faac511c4d66a8fed111b291",
        "e9d86e4dc34a985a7ec75a6f54a75ce45139e45240b386ab711db791bcd98718a13baf218c24493646680756cb50a6cbee158e2521449912301c0d4149111845",
        "c191fa3b550b391a7cb0728376277295e61c654f0bef2f58dce5c962a10b7dd75f06015465e55076e466263eebcaed20d2ebab39313e8bc567320fe8b2dc62b3");
}

slice<byte> hashOf(crypto::Hash h, std::string_view msg) {
    auto d = crypto::New(h);
    d->Write(as_bytes(msg));
    return d->Sum(slice<byte>());
}

TEST(GoRSA, SignPKCS1v15) {
    rsa::detail::AllowSmallKeys allowSmall;  // GODEBUG=rsa1024min=0
    // signPKCS1v15Tests: `openssl rsautl -verify -inkey pk -in signature | hexdump -C`
    const char* want =
        "a4f3fa6ea93bcdd0c57be020c1193ecbfd6f200a3d95c409769b029578fa0e336ad9a347600e40d3ae823b8c7e6bad88cc07c1d54c3a1523cbbb6d58efc362ae";
    auto key = test512Key();
    auto digest = hashOf(crypto::Hash::SHA1, "Test.\n");
    auto [s, err] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA1, digest);
    ASSERT_FALSE(err) << err->Error().view();
    EXPECT_EQ(std::string(hex::EncodeToString(s).view()), want);
    // Through the Signer interface, which takes a hash for PKCS #1 v1.5.
    auto [s2, err2] = key->Sign(nullptr, digest, crypto::HashOpts{crypto::Hash::SHA1});
    ASSERT_FALSE(err2);
    EXPECT_EQ(std::string(hex::EncodeToString(s2).view()), want);
}

TEST(GoRSA, VerifyPKCS1v15) {
    rsa::detail::AllowSmallKeys allowSmall;  // GODEBUG=rsa1024min=0
    auto key = test512Key();
    auto digest = hashOf(crypto::Hash::SHA1, "Test.\n");
    auto [sig, herr] = hex::DecodeString(
        "a4f3fa6ea93bcdd0c57be020c1193ecbfd6f200a3d95c409769b029578fa0e336ad9a347600e40d3ae823b8c7e6bad88cc07c1d54c3a1523cbbb6d58efc362ae");
    ASSERT_FALSE(herr);
    error err = rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA1, digest, sig);
    EXPECT_FALSE(err);

    // A different message, and a flipped bit, fail.
    auto other = hashOf(crypto::Hash::SHA1, "Test!\n");
    EXPECT_EQ(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA1, other, sig), rsa::ErrVerification);
    sig[3] ^= 1;
    EXPECT_EQ(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA1, digest, sig), rsa::ErrVerification);
    // A digest that isn't one.
    error bad = rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA1, as_bytes("short"), sig);
    ASSERT_TRUE(bad);
    EXPECT_EQ(rsa::ErrVerification->Error().view(), "crypto/rsa: verification error");
}

TEST(GoRSA, UnpaddedSignature) {
    rsa::detail::AllowSmallKeys allowSmall;  // GODEBUG=rsa1024min=0
    auto key = test512Key();
    auto msg = as_bytes("Thu Dec 19 18:06:16 EST 2013\n");
    // `openssl rsautl -sign -inkey key -in msg`
    auto [expected, derr] = base64::StdEncoding.DecodeString(
        "pX4DR8azytjdQ1rtUiC040FjkepuQut5q2ZFX1pTjBrOVKNjgsCDyiJDGZTCNoh9qpXYbhl7iEym30BWWwuiZg==");
    ASSERT_FALSE(derr);
    auto [sig, err] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash{}, msg);
    ASSERT_FALSE(err);
    EXPECT_EQ(std::string(hex::EncodeToString(sig).view()), std::string(hex::EncodeToString(expected).view()));
    EXPECT_FALSE(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash{}, msg, sig));
}

TEST(GoRSA, SignPKCS1v15NotHashed) {
    rsa::detail::AllowSmallKeys allowSmall;  // GODEBUG=rsa1024min=0
    auto key = test512Key();
    auto [s, err] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, as_bytes("too short"));
    EXPECT_FALSE(s.data());
    ASSERT_TRUE(err);
    EXPECT_EQ(err->Error().view(), "crypto/rsa: input must be hashed message");
}

TEST(GoRSA, PSSSigning) {
    struct {
        int signSaltLength, verifySaltLength;
        bool good;
    } tests[] = {
        {rsa::PSSSaltLengthAuto, rsa::PSSSaltLengthAuto, true},
        {rsa::PSSSaltLengthEqualsHash, rsa::PSSSaltLengthAuto, true},
        {rsa::PSSSaltLengthEqualsHash, rsa::PSSSaltLengthEqualsHash, true},
        {rsa::PSSSaltLengthEqualsHash, 8, false},
        {8, 8, true},
        {8, rsa::PSSSaltLengthAuto, true},
        {42, rsa::PSSSaltLengthAuto, true},
        {rsa::PSSSaltLengthAuto, 106, true},
        {rsa::PSSSaltLengthAuto, 20, false},
        {rsa::PSSSaltLengthAuto, -2, false},
    };
    auto key = test1024Key();
    auto hash = crypto::Hash::SHA1;
    auto hashed = hashOf(hash, "testing");
    rsa::PSSOptions opts;
    int i = 0;
    for (auto& t : tests) {
        SCOPED_TRACE(i++);
        opts.SaltLength = t.signSaltLength;
        auto [sig, err] = rsa::SignPSS(nullptr, *key, hash, hashed, &opts);
        ASSERT_FALSE(err) << err->Error().view();
        opts.SaltLength = t.verifySaltLength;
        error verr = rsa::VerifyPSS(key->PublicKey, hash, hashed, sig, &opts);
        EXPECT_EQ(!verr, t.good);
    }
}

TEST(GoRSA, PSSSignerAndHashOverride) {
    // Signer.Sign with a PSSOptions signs with PSS, and its Hash overrides
    // the hash argument of SignPSS.
    auto key = test1024Key();
    auto hashed = hashOf(crypto::Hash::SHA256, "testing");
    rsa::PSSOptions opts;
    opts.Hash = crypto::Hash::SHA256;
    EXPECT_EQ(opts.HashFunc(), crypto::Hash::SHA256);
    auto [sig, err] = key->Sign(nullptr, hashed, opts);
    ASSERT_FALSE(err);
    EXPECT_FALSE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sig, &opts));
    // A PKCS #1 v1.5 verifier rejects it.
    EXPECT_TRUE(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA256, hashed, sig));
    // SignPSS takes opts.Hash over its hash argument, VerifyPSS does not.
    auto [sig2, err2] = rsa::SignPSS(nullptr, *key, crypto::Hash::SHA1, hashed, &opts);
    ASSERT_FALSE(err2);
    rsa::PSSOptions other;
    other.Hash = crypto::Hash::SHA1;
    EXPECT_FALSE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sig2, &other));
    EXPECT_TRUE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA1, hashed, sig2, &opts));
}

TEST(GoRSA, PSSShortSignature) {
    // A signature without its leading zero byte is not valid, though
    // OpenSSL would accept it for PSS.
    auto key = test1024Key();
    for (int i = 0; i < 20000; i++) {
        auto hashed = hashOf(crypto::Hash::SHA256, "message " + std::to_string(i));
        auto [sig, err] = rsa::SignPSS(nullptr, *key, crypto::Hash::SHA256, hashed, nullptr);
        ASSERT_FALSE(err);
        if (sig[0] != 0) continue;
        EXPECT_FALSE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sig, nullptr));
        EXPECT_EQ(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, std::span<const byte>(sig).subspan(1), nullptr),
                  rsa::ErrVerification);
        return;
    }
    FAIL() << "no signature began with a zero byte";
}

TEST(GoRSA, SmallKeysRejected) {
    auto key = test512Key();
    const char* want = "crypto/rsa: 512-bit keys are insecure (see https://go.dev/pkg/crypto/rsa#hdr-Minimum_key_size)";
    auto hashed = hashOf(crypto::Hash::SHA256, "x");
    std::string sig(64, 'x');
    auto sigb = as_bytes(sig);
    auto [s1, e1] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, hashed);
    ASSERT_TRUE(e1);
    EXPECT_EQ(e1->Error().view(), want);
    auto [s2, e2] = rsa::SignPSS(nullptr, *key, crypto::Hash::SHA256, hashed, nullptr);
    ASSERT_TRUE(e2);
    EXPECT_EQ(e2->Error().view(), want);
    error e3 = rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA256, hashed, sigb);
    ASSERT_TRUE(e3);
    EXPECT_EQ(e3->Error().view(), want);
    error e4 = rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sigb, nullptr);
    ASSERT_TRUE(e4);
    EXPECT_EQ(e4->Error().view(), want);
    {
        rsa::detail::AllowSmallKeys allow;
        EXPECT_FALSE(rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, hashed).second);
    }
    EXPECT_TRUE(rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, hashed).second);
}

TEST(GoRSA, PSSNilOpts) {
    auto key = test1024Key();
    auto hashed = hashOf(crypto::Hash::SHA256, "testing");
    auto [sig, err] = rsa::SignPSS(nullptr, *key, crypto::Hash::SHA256, hashed, nullptr);
    ASSERT_FALSE(err);
    EXPECT_FALSE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sig, nullptr));
}

TEST(GoRSA, KeyGeneration) {
    auto [key, err] = rsa::GenerateKey(nullptr, 2048);
    ASSERT_FALSE(err);
    EXPECT_EQ(key->PublicKey.N.BitLen(), 2048);
    EXPECT_EQ(key->PublicKey.E, 65537);
    EXPECT_EQ(key->PublicKey.Size(), 256);
    EXPECT_EQ(key->Primes.size(), 2u);
    EXPECT_FALSE(key->Validate());

    auto digest = hashOf(crypto::Hash::SHA256, "hello");
    auto [sig, serr] = rsa::SignPKCS1v15(nullptr, *key, crypto::Hash::SHA256, digest);
    ASSERT_FALSE(serr);
    EXPECT_EQ(sig.size(), size_t{256});
    EXPECT_FALSE(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA256, digest, sig));
    sig[10] ^= 1;
    EXPECT_EQ(rsa::VerifyPKCS1v15(key->PublicKey, crypto::Hash::SHA256, digest, sig), rsa::ErrVerification);
    sig[10] ^= 1;

    // Public() is an equal key, and a copy of a key signs the same.
    auto pub = key->Public();
    EXPECT_TRUE(pub->Equal(key->PublicKey));
    EXPECT_FALSE(pub->Equal(test512Key()->PublicKey));
    rsa::PublicKey copy = key->PublicKey;
    EXPECT_TRUE(copy.Equal(key->PublicKey));
    EXPECT_FALSE(rsa::VerifyPKCS1v15(copy, crypto::Hash::SHA256, digest, sig));
}

TEST(GoRSA, KeyGenerationTooSmall) {
    auto [key, err] = rsa::GenerateKey(nullptr, 512);
    EXPECT_FALSE(key);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->Error().view(), "crypto/rsa: 512-bit keys are insecure (see https://go.dev/pkg/crypto/rsa#hdr-Minimum_key_size)");
}

TEST(GoRSA, ValidateBadKey) {
    // Go's TestModifiedPrivateKey: a private key that doesn't hold together.
    auto key = test512Key();
    EXPECT_FALSE(key->Validate());
    key->Primes[0].SetInt64(7);
    // The cache of the unchanged key was built at Validate; a fresh copy
    // is built from the new values.
    rsa::PrivateKey bad = *key;
    EXPECT_TRUE(bad.Validate());
}

TEST(GoRSA, PublicKeyEqual) {
    auto a = test512Key();
    auto b = test512Key();
    EXPECT_TRUE(a->PublicKey.Equal(b->PublicKey));
    b->PublicKey.E = 3;
    EXPECT_FALSE(a->PublicKey.Equal(b->PublicKey));
    EXPECT_FALSE(a->PublicKey.Equal(test1024Key()->PublicKey));
}


// ---- crypto/ecdsa ----

struct NamedCurve {
    const char* name;
    ptr<elliptic::Curve> c;
};
std::vector<NamedCurve> allCurves() {
    return {{"P224", elliptic::P224()}, {"P256", elliptic::P256()}, {"P384", elliptic::P384()},
            {"P521", elliptic::P521()}};
}

slice<byte> unhex(std::string_view h) {
    auto [b, err] = hex::DecodeString(string(h));
    EXPECT_FALSE(err);
    return b;
}

big::Int bigHex(std::string_view h) {
    big::Int z;
    z.SetString(h, 16);
    return z;
}

// Arithmetic that big::Int doesn't have, over OpenSSL's BIGNUM.
big::Int arith(const big::Int& a, const big::Int& b, bool sub) {
    pkey::BN z(BN_new());
    EXPECT_EQ((sub ? BN_sub : BN_add)(z.get(), pkey::bn(a), pkey::bn(b)), 1);
    char* h = BN_bn2hex(z.get());
    big::Int out = bigHex(h);
    OPENSSL_free(h);
    return out;
}
big::Int add(const big::Int& a, const big::Int& b) { return arith(a, b, false); }
big::Int sub(const big::Int& a, const big::Int& b) { return arith(a, b, true); }
big::Int neg(const big::Int& a) {
    big::Int zero;
    return sub(zero, a);
}

// A reader of zeros.
struct ZeroReader final : io::Reader {
    std::pair<isize, error> Read(std::span<byte> p) override {
        std::fill(p.begin(), p.end(), byte(0));
        return {static_cast<isize>(p.size()), nullptr};
    }
};

// A DER ECDSA signature from the encoded content of two INTEGERs (each with
// its own tag and length): for building signatures that are not canonical.
std::vector<byte> derSeq(const std::vector<byte>& inner) {
    std::vector<byte> out{0x30};
    if (inner.size() < 128) {
        out.push_back(static_cast<byte>(inner.size()));
    } else {
        out.push_back(0x81);
        out.push_back(static_cast<byte>(inner.size()));
    }
    out.insert(out.end(), inner.begin(), inner.end());
    return out;
}
std::vector<byte> derInt(std::vector<byte> v) {
    std::vector<byte> out{0x02, static_cast<byte>(v.size())};
    out.insert(out.end(), v.begin(), v.end());
    return out;
}
std::vector<byte> cat(std::vector<byte> a, const std::vector<byte>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
std::vector<byte> bytesOf(const slice<byte>& s) { return std::vector<byte>(s.data(), s.data() + s.len()); }

TEST(GoECDSA, SignAndVerify) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, gerr] = ecdsa::GenerateKey(c, nullptr);
        ASSERT_FALSE(gerr);
        slice<byte> hashed = unhex(hex::EncodeToString(as_bytes("testing")).view());
        auto [r, s, err] = ecdsa::Sign(&rand::Reader, *priv, hashed);
        ASSERT_FALSE(err) << err->Error().view();
        EXPECT_TRUE(ecdsa::Verify(priv->PublicKey, hashed, r, s));
        hashed[0] ^= 0xff;
        EXPECT_FALSE(ecdsa::Verify(priv->PublicKey, hashed, r, s)) << "Verify always works!";
    }
}

TEST(GoECDSA, SignAndVerifyASN1) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, gerr] = ecdsa::GenerateKey(c, &rand::Reader);
        ASSERT_FALSE(gerr);
        slice<byte> hashed = unhex(hex::EncodeToString(as_bytes("testing")).view());
        auto [sig, err] = ecdsa::SignASN1(&rand::Reader, *priv, hashed);
        ASSERT_FALSE(err) << err->Error().view();
        EXPECT_TRUE(ecdsa::VerifyASN1(priv->PublicKey, hashed, sig));
        hashed[0] ^= 0xff;
        EXPECT_FALSE(ecdsa::VerifyASN1(priv->PublicKey, hashed, sig)) << "VerifyASN1 always works!";
    }
}

TEST(GoECDSA, KeyGeneration) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, err] = ecdsa::GenerateKey(c, nullptr);
        ASSERT_FALSE(err);
        EXPECT_TRUE(priv->PublicKey.Curve == c);
        EXPECT_GT(priv->D.Sign(), 0);
        EXPECT_LT(priv->D.Cmp(c->Params().N), 0);
        EXPECT_GT(priv->PublicKey.X.Sign(), 0);
        EXPECT_GT(priv->PublicKey.Y.Sign(), 0);
        EXPECT_LE(priv->PublicKey.X.BitLen(), c->Params().BitSize);
    }
}

TEST(GoECDSA, NonceSafety) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        ZeroReader zero;
        auto [priv, gerr] = ecdsa::GenerateKey(c, &rand::Reader);
        ASSERT_FALSE(gerr);
        auto [r0, s0, err0] = ecdsa::Sign(&zero, *priv, unhex("74657374696e67"));
        ASSERT_FALSE(err0);
        auto [r1, s1, err1] = ecdsa::Sign(&zero, *priv, unhex("74657374696e672e2e2e"));
        ASSERT_FALSE(err1);
        EXPECT_NE(s0.Cmp(s1), 0) << "the signatures on two different messages were the same";
        EXPECT_NE(r0.Cmp(r1), 0) << "the nonce used for two different messages was the same";
    }
}

TEST(GoECDSA, INDCCA) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, gerr] = ecdsa::GenerateKey(c, &rand::Reader);
        ASSERT_FALSE(gerr);
        auto h = unhex("74657374696e67");
        auto [r0, s0, err0] = ecdsa::Sign(&rand::Reader, *priv, h);
        auto [r1, s1, err1] = ecdsa::Sign(&rand::Reader, *priv, h);
        ASSERT_FALSE(err0);
        ASSERT_FALSE(err1);
        EXPECT_NE(s0.Cmp(s1), 0) << "two signatures of the same message produced the same result";
        EXPECT_NE(r0.Cmp(r1), 0) << "two signatures of the same message produced the same nonce";
    }
}

TEST(GoECDSA, ZeroHashSignature) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        std::vector<byte> zeroHash(64, byte(0));
        auto [priv, gerr] = ecdsa::GenerateKey(c, &rand::Reader);
        ASSERT_FALSE(gerr);
        auto [r, s, err] = ecdsa::Sign(&rand::Reader, *priv, zeroHash);
        ASSERT_FALSE(err);
        EXPECT_TRUE(ecdsa::Verify(priv->PublicKey, zeroHash, r, s));
        // And an empty hash.
        auto [r2, s2, err2] = ecdsa::Sign(&rand::Reader, *priv, std::span<const byte>());
        ASSERT_FALSE(err2);
        EXPECT_TRUE(ecdsa::Verify(priv->PublicKey, std::span<const byte>(), r2, s2));
    }
}

// TestZeroSignature, TestNegativeSignature, TestRPlusNSignature and
// TestRMinusNSignature, and s as well as r.
TEST(GoECDSA, BadScalars) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        std::vector<byte> zeroHash(64, byte(0));
        auto [priv, gerr] = ecdsa::GenerateKey(c, &rand::Reader);
        ASSERT_FALSE(gerr);
        const auto& pub = priv->PublicKey;
        const auto& N = c->Params().N;
        auto [r, s, err] = ecdsa::Sign(&rand::Reader, *priv, zeroHash);
        ASSERT_FALSE(err);
        ASSERT_TRUE(ecdsa::Verify(pub, zeroHash, r, s));

        big::Int zero;
        big::Int negR = neg(r), negS = neg(s);
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, zero, zero));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, zero, s));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, r, zero));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, negR, s));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, r, negS));
        big::Int rPlusN = add(r, N), sPlusN = add(s, N);
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, rPlusN, s));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, r, sPlusN));
        big::Int rMinusN = sub(r, N), sMinusN = sub(s, N);
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, rMinusN, s));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, r, sMinusN));
        // Exactly N is out of range too.
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, N, s));
        EXPECT_FALSE(ecdsa::Verify(pub, zeroHash, r, N));
    }
}

// RFC 6979, A.2.5: P-256 and SHA-256, message "sample".
TEST(GoECDSA, RFC6979Vector) {
    ecdsa::PublicKey pub;
    pub.Curve = elliptic::P256();
    pub.X = bigHex("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6");
    pub.Y = bigHex("7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299");
    auto h = hashOf(crypto::Hash::SHA256, "sample");
    big::Int r = bigHex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716");
    big::Int s = bigHex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8");
    EXPECT_TRUE(ecdsa::Verify(pub, h, r, s));
    // Another message, and a changed s, fail.
    EXPECT_FALSE(ecdsa::Verify(pub, hashOf(crypto::Hash::SHA256, "test"), r, s));
    big::Int one;
    one.SetInt64(1);
    big::Int s2 = add(s, one);
    EXPECT_FALSE(ecdsa::Verify(pub, h, r, s2));

    // The private key signs, with a signature that verifies, through Signer.
    ecdsa::PrivateKey priv;
    priv.PublicKey = pub;
    priv.D = bigHex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
    auto [sig, err] = priv.Sign(nullptr, h, crypto::HashOpts{crypto::Hash::SHA256});
    ASSERT_FALSE(err) << err->Error().view();
    EXPECT_TRUE(ecdsa::VerifyASN1(pub, h, sig));
    auto pubOf = priv.Public();
    EXPECT_TRUE(pubOf->Equal(pub));
}

TEST(GoECDSA, PublicKeyEqual) {
    auto [a, e1] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    auto [b, e2] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(e1);
    ASSERT_FALSE(e2);
    EXPECT_TRUE(a->PublicKey.Equal(a->PublicKey));
    EXPECT_TRUE(a->PublicKey.Equal(*a->Public()));
    EXPECT_FALSE(a->PublicKey.Equal(b->PublicKey));
    ecdsa::PublicKey other = a->PublicKey;
    other.Curve = elliptic::P384();
    EXPECT_FALSE(a->PublicKey.Equal(other));
    ed25519::PublicKey ed;
    EXPECT_FALSE(a->PublicKey.Equal(ed));
}

// What Go's Verify does with a public key that is not valid: false.
TEST(GoECDSA, InvalidPublicKeys) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, gerr] = ecdsa::GenerateKey(c, nullptr);
        ASSERT_FALSE(gerr);
        auto h = unhex("74657374696e67");
        auto [sig, err] = ecdsa::SignASN1(nullptr, *priv, h);
        ASSERT_FALSE(err);
        ASSERT_TRUE(ecdsa::VerifyASN1(priv->PublicKey, h, sig));

        big::Int one, zero;
        one.SetInt64(1);
        auto check = [&](const ecdsa::PublicKey& k, const char* what) {
            SCOPED_TRACE(what);
            EXPECT_FALSE(ecdsa::VerifyASN1(k, h, sig));
            auto [r, s, e2] = ecdsa::Sign(nullptr, *priv, h);
            EXPECT_FALSE(e2);
            EXPECT_FALSE(ecdsa::Verify(k, h, r, s));
        };
        ecdsa::PublicKey k = priv->PublicKey;
        k.Y = add(k.Y, one);
        check(k, "off the curve");
        k = priv->PublicKey;
        k.X = zero;
        k.Y = zero;
        check(k, "(0, 0), where infinity would be");
        k = priv->PublicKey;
        k.X = add(k.X, c->Params().P);
        check(k, "X not reduced");
        k = priv->PublicKey;
        k.Y = neg(k.Y);
        check(k, "negative Y");
        k = priv->PublicKey;
        k.X = neg(k.X);
        check(k, "negative X");
        k = priv->PublicKey;
        k.Curve = nullptr;
        check(k, "nil curve");
        // The point of one curve with another's.
        k = priv->PublicKey;
        k.Curve = c == elliptic::P256() ? elliptic::P384() : elliptic::P256();
        check(k, "mismatched curve");
        k = priv->PublicKey;
        k.X = bigHex("1");
        k.Y = bigHex("1");
        check(k, "(1, 1)");
    }
}

TEST(GoECDSA, UnsupportedCurve) {
    struct Custom final : elliptic::Curve {
        elliptic::CurveParams p;
        const elliptic::CurveParams& Params() const override { return p; }
    };
    auto c = make_ptr<Custom>();
    c->p = elliptic::P256()->Params();
    auto [priv, err] = ecdsa::GenerateKey(c, nullptr);
    EXPECT_TRUE(err);
    EXPECT_FALSE(priv);
    auto [good, gerr] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(gerr);
    auto h = unhex("74657374696e67");
    auto [sig, serr] = ecdsa::SignASN1(nullptr, *good, h);
    ASSERT_FALSE(serr);
    ecdsa::PublicKey k = good->PublicKey;
    k.Curve = c;
    EXPECT_FALSE(ecdsa::VerifyASN1(k, h, sig));
}

TEST(GoECDSA, MalformedDER) {
    for (auto& [name, c] : allCurves()) {
        SCOPED_TRACE(name);
        auto [priv, gerr] = ecdsa::GenerateKey(c, nullptr);
        ASSERT_FALSE(gerr);
        const auto& pub = priv->PublicKey;
        auto h = unhex("74657374696e67");
        auto [sig, err] = ecdsa::SignASN1(nullptr, *priv, h);
        ASSERT_FALSE(err);
        std::vector<byte> good = bytesOf(sig);
        ASSERT_TRUE(ecdsa::VerifyASN1(pub, h, good));

        auto bad = [&](const std::vector<byte>& v, const char* what) {
            SCOPED_TRACE(what);
            EXPECT_FALSE(ecdsa::VerifyASN1(pub, h, v));
        };
        bad({}, "empty");
        bad({0x30}, "a tag alone");
        bad({0x30, 0x00}, "an empty sequence");
        auto trailing = good;
        trailing.push_back(0);
        bad(trailing, "trailing byte");
        auto trailing2 = good;
        trailing2.insert(trailing2.end(), good.begin(), good.end());
        bad(trailing2, "two signatures");
        for (size_t cut : {size_t(1), good.size() / 2, good.size() - 1}) {
            bad(std::vector<byte>(good.begin(), good.begin() + cut), "truncated");
        }
        auto wrongTag = good;
        wrongTag[0] = 0x31;
        bad(wrongTag, "a SET");
        // The parts of the signature, to be reassembled.
        size_t off = good[1] & 0x80 ? 3 : 2;  // after the header of the SEQUENCE
        size_t nR = static_cast<size_t>(good[off + 1]);
        std::vector<byte> rBytes(good.begin() + off + 2, good.begin() + off + 2 + nR);
        size_t sOff = off + 2 + nR;
        size_t nS = static_cast<size_t>(good[sOff + 1]);
        std::vector<byte> sBytes(good.begin() + sOff + 2, good.begin() + sOff + 2 + nS);
        ASSERT_TRUE(ecdsa::VerifyASN1(pub, h, derSeq(cat(derInt(rBytes), derInt(sBytes)))));
        // A leading zero that isn't needed, in r.
        auto padded = rBytes;
        if (!(padded[0] & 0x80)) {
            padded.insert(padded.begin(), byte(0));
            bad(derSeq(cat(derInt(padded), derInt(sBytes))), "non-minimal r");
        }
        // A negative r: the same bytes with the sign bit set.
        auto neg = rBytes;
        neg[0] |= 0x80;
        bad(derSeq(cat(derInt(neg), derInt(sBytes))), "negative r");
        bad(derSeq(cat(derInt({}), derInt(sBytes))), "empty r");
        bad(derSeq(cat(derInt({0}), derInt(sBytes))), "r is 0");
        bad(derSeq(cat(derInt(rBytes), derInt({0}))), "s is 0");
        bad(derSeq(derInt(rBytes)), "r alone");
        bad(derSeq(cat(cat(derInt(rBytes), derInt(sBytes)), derInt({1}))), "three integers");
        auto wrongInt = cat(derInt(rBytes), derInt(sBytes));
        wrongInt[0] = 0x04;
        bad(derSeq(wrongInt), "an OCTET STRING for r");
        // r = N and s = N, as the longest integers.
        auto nBytes = bytesOf(c->Params().N.Bytes());
        if (nBytes[0] & 0x80) nBytes.insert(nBytes.begin(), byte(0));
        bad(derSeq(cat(derInt(nBytes), derInt(sBytes))), "r = N");
        bad(derSeq(cat(derInt(rBytes), derInt(nBytes))), "s = N");
        // Indefinite length (BER).
        std::vector<byte> ber{0x30, 0x80};
        auto inner = cat(derInt(rBytes), derInt(sBytes));
        ber.insert(ber.end(), inner.begin(), inner.end());
        ber.push_back(0);
        ber.push_back(0);
        bad(ber, "indefinite length");
        // A long form of the length that fits the short.
        std::vector<byte> longForm{0x30, 0x81, static_cast<byte>(inner.size())};
        longForm.insert(longForm.end(), inner.begin(), inner.end());
        if (inner.size() < 128) bad(longForm, "non-minimal length");
    }
}

// ---- crypto/ed25519 ----

TEST(GoEd25519, SignVerify) {
    ZeroReader zero;
    auto [pub, priv, gerr] = ed25519::GenerateKey(&zero);
    ASSERT_FALSE(gerr);
    ASSERT_EQ(static_cast<isize>(pub->key.len()), ed25519::PublicKeySize);
    ASSERT_EQ(static_cast<isize>(priv->key.len()), ed25519::PrivateKeySize);
    auto message = as_bytes("test message");
    auto sig = ed25519::Sign(*priv, message);
    EXPECT_EQ(static_cast<isize>(sig.len()), ed25519::SignatureSize);
    EXPECT_TRUE(ed25519::Verify(*pub, message, sig));
    EXPECT_FALSE(ed25519::Verify(*pub, as_bytes("wrong message"), sig)) << "signature of different message accepted";
    // The signature is deterministic, and the key from the zero seed is
    // RFC 8032's well known one.
    EXPECT_EQ(std::string(hex::EncodeToString(pub->key).view()),
              "3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29");
    auto sig2 = ed25519::Sign(*priv, message);
    EXPECT_EQ(std::string(hex::EncodeToString(sig).view()), std::string(hex::EncodeToString(sig2).view()));

    // Flipping any part of the signature breaks it, and so do wrong lengths.
    for (int i : {0, 31, 32, 63}) {
        auto bad = ed25519::Sign(*priv, message);
        bad[i] ^= 1;
        EXPECT_FALSE(ed25519::Verify(*pub, message, bad)) << i;
    }
    EXPECT_FALSE(ed25519::Verify(*pub, message, sig.Sub(0, 63)));
    EXPECT_FALSE(ed25519::Verify(*pub, message, std::span<const byte>()));
    auto longer = appendSpan(sig, std::span<const byte>(as_bytes("x")));
    EXPECT_FALSE(ed25519::Verify(*pub, message, longer));
    // Another key.
    auto [pub2, priv2, e2] = ed25519::GenerateKey(nullptr);
    ASSERT_FALSE(e2);
    EXPECT_FALSE(ed25519::Verify(*pub2, message, sig));
}

TEST(GoEd25519, CryptoSigner) {
    ZeroReader zero;
    auto [pub, priv, gerr] = ed25519::GenerateKey(&zero);
    ASSERT_FALSE(gerr);
    crypto::Signer& signer = *priv;
    auto pubIface = signer.Public();
    auto pub2 = dynamic_pointer_cast<ed25519::PublicKey>(pubIface);
    ASSERT_TRUE(pub2) << "expected PublicKey from Public()";
    EXPECT_EQ(std::string(hex::EncodeToString(pub->key).view()), std::string(hex::EncodeToString(pub2->key).view()));
    auto message = as_bytes("message");
    auto [sig, err] = signer.Sign(nullptr, message, crypto::HashOpts{});
    ASSERT_FALSE(err) << err->Error().view();
    EXPECT_TRUE(ed25519::Verify(*pub2, message, sig));
    // Ed25519ph isn't supported.
    auto [sig2, err2] = signer.Sign(nullptr, message, crypto::HashOpts{crypto::Hash::SHA512});
    EXPECT_TRUE(err2);
}

TEST(GoEd25519, Equal) {
    auto [pub, priv, err] = ed25519::GenerateKey(&rand::Reader);
    ASSERT_FALSE(err);
    EXPECT_TRUE(pub->Equal(*pub));
    EXPECT_TRUE(pub->Equal(*priv->Public()));
    auto [otherPub, otherPriv, err2] = ed25519::GenerateKey(&rand::Reader);
    ASSERT_FALSE(err2);
    EXPECT_FALSE(pub->Equal(*otherPub));
    EXPECT_FALSE(pub->Equal(*otherPriv->Public()));
    auto [ec, eerr] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(eerr);
    EXPECT_FALSE(pub->Equal(ec->PublicKey));
}

TEST(GoEd25519, MalleabilityRejected) {
    // RFC 8032 section 5.1.7 requires s in [0, order): a multiple of the
    // order added to s must not give a second valid signature.
    auto msg = unhex("54657374");
    auto sig = unhex(
        "7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab0"
        "67654bce3832c2d76f8f6f5dafc08d9339d4eef676573336a5c51eb6f946b31d");
    ed25519::PublicKey pub;
    pub.key = unhex("7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa");
    EXPECT_FALSE(ed25519::Verify(pub, msg, sig)) << "non-canonical signature accepted";
}

TEST(GoEd25519, Golden) {
    // The first 8 lines of Go's testdata/sign.input.gz: seed and public key,
    // public key, message, signature.
    struct {
        const char* priv;
        const char* pub;
        const char* msg;
        const char* sig;
    } tests[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
         "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
        {"0d4a05b07352a5436e180356da0ae6efa0345ff7fb1572575772e8005ed978e9e61a185bcef2613a6c7cb79763ce945d3b245d76114dd440bcf5f2dc1aa57057",
         "e61a185bcef2613a6c7cb79763ce945d3b245d76114dd440bcf5f2dc1aa57057",
         "cbc77b",
         "d9868d52c2bebce5f3fa5a79891970f309cb6591e3e1702a70276fa97c24b3a8e58606c38c9758529da50ee31b8219cba45271c689afa60b0ea26c99db19b00c"},
        {"6df9340c138cc188b5fe4464ebaa3f7fc206a2d55c3434707e74c9fc04e20ebbc0dac102c4533186e25dc43128472353eaabdb878b152aeb8e001f92d90233a7",
         "c0dac102c4533186e25dc43128472353eaabdb878b152aeb8e001f92d90233a7",
         "5f4c8989",
         "124f6fc6b0d100842769e71bd530664d888df8507df6c56dedfdb509aeb93416e26b918d38aa06305df3095697c18b2aa832eaa52edc0ae49fbae5a85e150c07"},
        {"b780381a65edf8b78f6945e8dbec7941ac049fd4c61040cf0c324357975a293ce253af0766804b869bb1595be9765b534886bbaab8305bf50dbc7f899bfb5f01",
         "e253af0766804b869bb1595be9765b534886bbaab8305bf50dbc7f899bfb5f01",
         "18b6bec097",
         "b2fc46ad47af464478c199e1f8be169f1be6327c7f9a0a6689371ca94caf04064a01b22aff1520abd58951341603faed768cf78ce97ae7b038abfe456aa17c09"},
        {"78ae9effe6f245e924a7be63041146ebc670dbd3060cba67fbc6216febc44546fbcfbfa40505d7f2be444a33d185cc54e16d615260e1640b2b5087b83ee3643d",
         "fbcfbfa40505d7f2be444a33d185cc54e16d615260e1640b2b5087b83ee3643d",
         "89010d855972",
         "6ed629fc1d9ce9e1468755ff636d5a3f40a5d9c91afd93b79d241830f7e5fa29854b8f20cc6eecbb248dbd8d16d14e99752194e4904d09c74d639518839d2300"},
        {"691865bfc82a1e4b574eecde4c7519093faf0cf867380234e3664645c61c5f7998a5e3a36e67aaba89888bf093de1ad963e774013b3902bfab356d8b90178a63",
         "98a5e3a36e67aaba89888bf093de1ad963e774013b3902bfab356d8b90178a63",
         "b4a8f381e70e7a",
         "6e0af2fe55ae377a6b7a7278edfb419bd321e06d0df5e27037db8812e7e3529810fa5552f6c0020985ca17a0e02e036d7b222a24f99b77b75fdd16cb05568107"},
    };
    int line = 0;
    for (auto& t : tests) {
        SCOPED_TRACE(++line);
        auto privBytes = unhex(t.priv);
        auto pubKey = unhex(t.pub);
        auto msg = unhex(t.msg);
        auto sig = unhex(t.sig);
        ed25519::PrivateKey priv;
        priv.key = privBytes;
        ed25519::PublicKey pub;
        pub.key = pubKey;
        auto sig2 = ed25519::Sign(priv, msg);
        EXPECT_EQ(std::string(hex::EncodeToString(sig).view()), std::string(hex::EncodeToString(sig2).view()));
        EXPECT_TRUE(ed25519::Verify(pub, msg, sig2));
        auto priv2 = ed25519::NewKeyFromSeed(privBytes.Sub(0, 32));
        EXPECT_EQ(std::string(hex::EncodeToString(priv2->key).view()), t.priv);
        EXPECT_EQ(std::string(hex::EncodeToString(priv2->Seed()).view()), std::string(t.priv).substr(0, 64));
        auto p2 = dynamic_pointer_cast<ed25519::PublicKey>(priv2->Public());
        ASSERT_TRUE(p2);
        EXPECT_EQ(std::string(hex::EncodeToString(p2->key).view()), t.pub);
    }
}

// Go panics on a public key of the wrong length; any other signature
// length is just false.
TEST(GoEd25519, BadLengths) {
    ed25519::PublicKey pub;
    pub.key = slice<byte>::make(31);
    auto sig = slice<byte>::make(64);
    EXPECT_DEATH((void)ed25519::Verify(pub, as_bytes("m"), sig), "bad public key length: 31");
    pub.key = slice<byte>::make(32);
    EXPECT_FALSE(ed25519::Verify(pub, as_bytes("m"), sig));  // zeros: not a signature
    EXPECT_FALSE(ed25519::Verify(pub, as_bytes("m"), slice<byte>::make(65)));
    EXPECT_DEATH((void)ed25519::NewKeyFromSeed(slice<byte>::make(31)), "bad seed length: 31");
    ed25519::PrivateKey priv;
    priv.key = slice<byte>::make(63);
    EXPECT_DEATH((void)ed25519::Sign(priv, as_bytes("m")), "bad private key");
}

// A public key that isn't a point of the curve: Verify is false, not a crash.
TEST(GoEd25519, InvalidPublicKey) {
    ed25519::PublicKey pub;
    pub.key = unhex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    auto [pk, sk, err] = ed25519::GenerateKey(nullptr);
    ASSERT_FALSE(err);
    auto sig = ed25519::Sign(*sk, as_bytes("m"));
    EXPECT_FALSE(ed25519::Verify(pub, as_bytes("m"), sig));
    // The all-zero key (y = 0) isn't a valid signer of a real signature.
    pub.key = slice<byte>::make(32);
    EXPECT_FALSE(ed25519::Verify(pub, as_bytes("m"), sig));
}

}  // namespace
