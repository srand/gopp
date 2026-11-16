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
#include <gopp/crypto.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/hex.hpp>

#include <gtest/gtest.h>

#include "crypto/rsa_internal.hpp"

#include <string>

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
    auto [sig2, err2] = rsa::SignPSS(nullptr, *key, crypto::Hash::SHA1, hashed, &opts);
    ASSERT_FALSE(err2);
    EXPECT_FALSE(rsa::VerifyPSS(key->PublicKey, crypto::Hash::SHA256, hashed, sig2, &opts));
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

}  // namespace
