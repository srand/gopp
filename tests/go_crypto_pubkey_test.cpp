// Tests of go++'s crypto, math/big and crypto/elliptic, which crypto/rsa,
// crypto/ecdsa and crypto/ed25519 build on. Go has no such tests of its own:
// crypto's hash identifiers are tested through the packages that register
// them, and the curve parameters through the curves. So these check the
// values Go's crypto.go, params.go and nistec.go give: each Hash's value,
// name and size, and each curve's name, bit size and parameters.
#include <gopp/crypto.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/encoding/hex.hpp>

#include <gtest/gtest.h>

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

}  // namespace
