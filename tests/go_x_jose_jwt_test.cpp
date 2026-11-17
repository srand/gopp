// Ported from github.com/go-jose/go-jose/v4's jwt/validation_test.go,
// jwt/claims_test.go, jwt/builder_test.go and jwt/jwt_test.go (v4.1.7;
// Apache 2.0, see x/jose/LICENSE), with the names of their tests:
// TestFieldsMatch, TestExpiryAndNotBefore, TestIssuedInFuture,
// TestOptionalDateClaims; TestEncodeClaims, TestEncodeClaimsWithSingleAudience,
// TestDecodeClaims, TestNumericDate, TestNumericDateOutOfRange,
// TestOutOfRangeNbfNotBeforeCheck, TestEncodeClaimsTimeValues;
// TestIntegerAndFloatsNormalize, TestBuilderCustomClaimsNonPointer,
// TestBuilderCustomClaimsPointer, TestBuilderMergeClaims,
// TestBuilderHeadersSigner; TestGetClaimsWithoutVerification,
// TestDecodeTokenWithJWKS, TestDecodeTokenWithMismatchedJWKSKID,
// TestDecodeToken and TestTamperedJWT.
//
// go++ has no symmetric keys and no JWE: Go's HMAC-signed tokens are signed
// with RSA here, and the encrypted tokens of TestGetClaimsWithoutVerification,
// TestDecodeToken and TestTamperedJWT are left out (TestTamperedJWT tampers
// with a signed token instead), as are TestBuilderSignedAndEncrypted,
// TestBuilderHeadersEncrypter and TestValidateKeyEncryptionAlgorithm.
// Expected.WithTime is Expected{.Time = t}. The test keys are Go's, from the
// parameters of their PEM.
#include <gopp/crypto/rand.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/jose.hpp>
#include <gopp/jose/jwt.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace gopp;
namespace jose = gopp::jose;
namespace jwt = gopp::jose::jwt;

namespace {

using Algs = std::vector<std::string_view>;

big::Int intFromHex(std::string_view h) {
    auto [b, err] = hex::DecodeString(h);
    EXPECT_FALSE(err);
    big::Int n;
    n.SetBytes(b);
    return n;
}

ptr<rsa::PrivateKey> rsaKey(std::string_view n, std::string_view d, std::string_view p, std::string_view q) {
    auto k = make_ptr<rsa::PrivateKey>();
    k->PublicKey.N = intFromHex(n);
    k->PublicKey.E = 65537;
    k->D = intFromHex(d);
    k->Primes.push_back(intFromHex(p));
    k->Primes.push_back(intFromHex(q));
    EXPECT_FALSE(k->Validate());
    return k;
}

const ptr<rsa::PrivateKey>& testPrivRSAKey1() {
    static const auto k = rsaKey(
        "c81c1bc31c0afb8e1f21c5aa810a5d757e323d619b5d0b96f71ed5d35d95b773674838898d5237df4fc3ce3faa793a9ce2be60b5a26ee23b"
        "2b99de72630bd2d3df3aaab377f68b97789889b112ea86eae59daf215f929d2e22fe5592354efa241677aa4300f18feebd597e85c295ec82"
        "b66c8d1f82529099de34f3a4ff80713bcbdcbd19c2313fc6bfcd484fa1fe390e8ea2f58eb89afc457f6dd70b828c2f5ecd9c5e23d38d7df8"
        "623b914cad58791fd2c94e5ddd3ce55ae1a11c3af81de398419afc40c97a9ff1d684e8a9763bbbd0e2ec209d24d55d82088c28db7a9b9eae"
        "c02920c1b1fd73d28225352b75ed6a1fd306f7a9f92dd59703dcabf407db94bd",
        "7becbc5ca61f3f0e12c58d6e3c1fb9252c13dce3779db5b1b63488632f4faa9e7355cc7d92e159ec97af4129385f7f26ed5cccf36f36902f"
        "e89e8f9df32cbd51acaade4fea78ea95d17f55b62d6ac446f0102906d7f32a180a5fcb8a922ce8c83489c30d5fa63966b628982c2a0742c2"
        "e84a6d5318181da8cc6daa7d82257ef958bcb7011291969acc091cd9a47234ed1852322603dbb08ea776490e7906ea40a01a69d5ed1b5c54"
        "cc7faf22fa9a525fe502435ffa9e2a84f907b280165dfd6abbd7186b64fc4fe984a3bf40c258150ba6b05d058d4538b20a50c90bb72ee8d0"
        "726475c22c531b6a5cd617da9e23638f6855f6c772015ab6f20f1709e1349d41",
        "ff169fee4289cc061d61e158b81e5c5976a48e02b4e2b5755a95d2c5195276dcc9e04c657f4ade161fa791a782027cfca576c7dcbfa319a4"
        "dc0d9b5467941e46b69aa96f2d5db2b3927ef55b63b537be92e777cfa127f953e73098b79128af8e8dea25919185b335ee0f8b1885cf5a6e"
        "194bc509b65d79e00256b3cb996dc929",
        "c8d32f564dff1ecca3dbb5aa7850413ac817f8f5fb688551b3a78883e8d835baa78fc0a6a1c2855728e735efd0a3186b56fd45013910282"
        "919b17476a31eae8b260b01c69680473db508e3f2f7d054ca9f444793fcbd89fade50bb0a977f2bbbb23236b3d43033601648795647230f"
        "0ef70b863492a3445cbd499c83dd5d1d75");
    return k;
}

const ptr<rsa::PrivateKey>& testPrivRSAKey2() {
    static const auto k = rsaKey(
        "b1274f639176793a4caa16b10aaecf864f3651be82f38b2cc34c864b242e8156bc9abb881a163b811bea679b8cf9de80cfd175678c5b31d6"
        "607047a4477979576f12d7b0c7c9a4ccd4f48d9245e9fd0e89290f5f3b0809e32cea0df50465d61f0203eb2a479270f6676599d681282f7d"
        "33d41800f2b7bc635f3c2e2ca28f0b0d20627a69ade6b68be1649fa1887feaa839ac7513ca6636f2ec557b0dc8f58a7ede5b48c3e5a5443b"
        "56e65f8c3390925287e38f6cdc81c1b72c4b8fb6fc99f8248e6f1c5c659ba09bdcdc25d89accdc94c6d5fcc808f75e0f0775ed23b744035e"
        "fec0f94d0a3158bd957e8f24468d8f8b4e84e99084954ccc0cabd56fdb89f655",
        "76e2a79f6d463192d3522e0a3fde12bcd2b9e45fd2a7b855a0f6e104da522f505320100c055db82f2bd9c2101caaaf0c88eacb3c636febe1"
        "2f350a8f0fbc5097419e468b87782615a4303e35d49f73c8e6795cac8f04150694e4b9db1d496e49bee37c5512550f63fcfdad20cbea78fc"
        "2fe4fcfadde83a10022073e8566b62811d8988087a01ce31759de2fccedc434e59221423a722ec0ae8f98d19ef991f895a2400685470582b"
        "679f04f367c1621f087313b6d28327b9c1ab2e2710ce3e77ea3c78c17dc275de23afd515109f59208d65765a74de766514b5ea247d79e3b0"
        "2a64cfb7bd57011a827c4fd650e150c02fefb42b60cb0bd2644c0440602122fd",
        "de6c8735fb6cdb96142d550fde21b104ad9701eeff93914a11a017d1d258fb6ca04526045edfb7e0bb5c5ff79feceb7a247673b0fceb90aa"
        "d1a6831fc94cbdf250b05d35cc130919b8d6df9caa81b29a32b45516ea93a096048774137af94da09559e764d892365f04ef209d407838aa"
        "7c97831d40f8e326a89f06f2b23f590f",
        "cbe5541f4e9a556899363d7370840911424959f11166b7fee9c80ddd75d40d770324e0597c17b42e3b5801c43887767c08ae510ecfa7d3b0"
        "cdcbc084be435d8fd6f605221a68ec70c1c2f8df33008fc41212e23236f1181c01fd156d9ffe087026e9b6fd883c2c8d7861d49e683a6e55"
        "2a78eeebfacf1ff418fa6778d299d25b");
    return k;
}

ptr<jose::Signer> mustMakeSigner(std::string_view alg, ptr<object> k) {
    jose::SignerOptions opts;
    opts.WithType("JWT");
    auto [sig, err] = jose::NewSigner({alg, std::move(k)}, &opts);
    if (err) panic("failed to create signer:" + err.Error());
    return sig;
}

const ptr<jose::Signer>& rsaSigner() {
    static const auto s = mustMakeSigner(jose::RS256, testPrivRSAKey1());
    return s;
}

// 2016-01-01T00:00:00Z
time::Time date2016() { return time::Unix(1451606400, 0); }

struct testClaims {
    [[= json::tag("sub")]] string Subject;
};

struct invalidMarshalClaims {
    std::pair<slice<byte>, error> MarshalJSON() const { return {slice<byte>(), errors::New("failed marshaling invalid claims")}; }
};

struct numberClaims {
    [[= json::tag("int")]] int64 Int;
    [[= json::tag("float")]] double Float;
};

struct customClaims {
    [[= json::tag("scopes,omitempty")]] std::vector<string> Scopes;
};

const char* const rsaSignedToken =
    "eyJhbGciOiJSUzI1NiJ9.eyJpc3MiOiJpc3N1ZXIiLCJzY29wZXMiOlsiczEiLCJzMiJdLCJzdWIiOiJzdWJqZWN0In0."
    "UDDtyK9gC9kyHltcP7E_XODsnqcJWZIiXeGmSAH7SE9YKy3N0KSfFIN85dCNjTfs6zvy4rkrCHzLB7uKAtzMearh3q7jL4nxbhUMhlUcs_9QDVoN4q"
    "_j58XmRqBqRnBk-RmDu9TgcV8RbErP4awpIhwWb5UU-hR__4_iNbHdKqwSUPDKYGlf5eicuiYrPxH8mxivk4LRD-vyRdBZZKBt0XIDnEU4TdcNCzA"
    "XojkftqcFWYsczwS8R4JHd1qYsMyiaWl4trdHZkO4QkeLe34z4ZAaPMt3wE-gcU-VoqYTGxz-K3Le2VaZ0r3j_z6bOInsv0yngC_cD1dCXMyQJWnWjQ";
const char* const rsaSignedTokenWithKid =
    "eyJhbGciOiJSUzI1NiIsImtpZCI6ImZvb2JhciJ9.eyJpc3MiOiJpc3N1ZXIiLCJzY29wZXMiOlsiczEiLCJzMiJdLCJzdWIiOiJzdWJqZWN0In0."
    "RxZhTRfPDb6UJ58FwvC89GgJGC8lAO04tz5iLlBpIJsyPZB0X_UgXSj0SGVFm2jbP_i-ZVH4HFC2fMB1n-so9CnCOpunWwhYNdgF6ewQJ0ADTWwfDG"
    "sK12UOmyT2naaZN8ZUBF8cgPtOgdWqQjk2Ng9QFRJxlUuKYczBp7vjWvgX8WMwQcaA-eK7HtguR4e9c4FMbeFK8Soc4jCsVTjIKdSn9SErc42gFu6"
    "5NI1hZ3OPe_T7AZqdDjCkJpoiJ65GdD_qvGkVndJSEcMp3riXQpAy0JbctVkYecdFaGidbxHRrdcQYHtKn-XGMCh2uoBKleUr1fTMiyCGPQQesy3xH"
    "w";
const char* const invalidPayloadSignedToken =
    "eyJhbGciOiJIUzI1NiJ9.aW52YWxpZC1wYXlsb2Fk.ScBKKm18jcaMLGYDNRUqB5gVMRZl4DM6dh3ShcxeNgY";
const char* const invalidPartsSignedToken =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJzdWJqZWN0IiwiaXNzIjoiaXNzdWVyIiwic2NvcGVzIjpbInMxIiwiczIiXX0";

// Go's hmacSignedToken, signed with RSA.
const std::string& signedToken() {
    static const std::string tok = [] {
        jwt::Claims c;
        c.Subject = "subject";
        c.Issuer = "issuer";
        customClaims cc;
        cc.Scopes = {"s1", "s2"};
        auto [s, err] = jwt::Signed(rsaSigner()).Claims(c).Claims(cc).Serialize();
        if (err) panic(err.Error());
        return std::string(s);
    }();
    return tok;
}

std::map<string, json::Value> subjectIssuerScopes() {
    return {{"sub", "subject"}, {"iss", "issuer"}, {"scopes", json::Value::array{"s1", "s2"}}};
}

std::vector<std::string> vec(const jwt::Audience& a) { return std::vector<std::string>(a.begin(), a.end()); }

bool contains(const error& err, std::string_view sub) {
    return err && std::string_view(err.Error()).find(sub) != std::string_view::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// validation_test.go

TEST(GoJWT, FieldsMatch) {
    jwt::Claims c;
    c.Issuer = "issuer";
    c.Subject = "subject";
    c.Audience = jwt::Audience{"a1", "a2"};
    c.ID = "42";

    std::vector<jwt::Expected> valid = {
        {.Issuer = "issuer"},
        {.Subject = "subject"},
        {.AnyAudience = jwt::Audience{"a1", "a2"}},
        {.AnyAudience = jwt::Audience{"a2", "a1"}},
        {.AnyAudience = jwt::Audience{"a1"}},
        {.AnyAudience = jwt::Audience{"a2"}},
        {.ID = "42"},
    };
    for (const auto& v : valid) EXPECT_FALSE(c.Validate(v));

    std::vector<std::pair<jwt::Expected, error>> invalid = {
        {{.Issuer = "invalid-issuer"}, jwt::ErrInvalidIssuer},
        {{.Subject = "invalid-subject"}, jwt::ErrInvalidSubject},
        {{.AnyAudience = jwt::Audience{"invalid-audience"}}, jwt::ErrInvalidAudience},
        {{.AnyAudience = jwt::Audience{"invalid-audience", "invalid2"}}, jwt::ErrInvalidAudience},
        {{.ID = "invalid-id"}, jwt::ErrInvalidID},
    };
    for (const auto& [e, want] : invalid) EXPECT_EQ(c.Validate(e), want);
}

TEST(GoJWT, ExpiryAndNotBefore) {
    auto now = time::Unix(1451649600, 0);  // 2016-01-01T12:00:00Z
    auto twelveHoursAgo = now.Add(-12 * time::Hour);

    jwt::Claims c;
    c.IssuedAt = jwt::NewNumericDate(twelveHoursAgo);
    c.NotBefore = jwt::NewNumericDate(twelveHoursAgo);
    c.Expiry = jwt::NewNumericDate(now);

    // expired - default leeway (1 minute)
    EXPECT_FALSE(c.Validate({.Time = now}));
    EXPECT_EQ(c.Validate({.Time = now.Add(2 * jwt::DefaultLeeway)}), jwt::ErrExpired);
    // some error is okay (leeway)
    EXPECT_FALSE(c.Validate({.Time = now.Add(jwt::DefaultLeeway)}));

    // expired - no leeway
    EXPECT_FALSE(c.ValidateWithLeeway({.Time = now}, time::Duration()));
    EXPECT_EQ(c.ValidateWithLeeway({.Time = now.Add(time::Second)}, time::Duration()), jwt::ErrExpired);

    // not before - default leeway (1 minute)
    EXPECT_FALSE(c.Validate({.Time = twelveHoursAgo}));
    EXPECT_EQ(c.Validate({.Time = twelveHoursAgo.Add(-2 * jwt::DefaultLeeway)}), jwt::ErrNotValidYet);
    // some error is okay (leeway)
    EXPECT_FALSE(c.Validate({.Time = twelveHoursAgo.Add(-jwt::DefaultLeeway)}));
}

TEST(GoJWT, IssuedInFuture) {
    auto now = time::Unix(1451649600, 0);
    auto oneHourInThePast = now.Add(-time::Hour);
    auto oneHourInTheFuture = now.Add(time::Hour);

    jwt::Claims c;
    c.IssuedAt = jwt::NewNumericDate(now);
    c.NotBefore = jwt::NewNumericDate(oneHourInThePast);
    c.Expiry = jwt::NewNumericDate(oneHourInTheFuture);

    // defaults: valid right now, or in the future
    EXPECT_FALSE(c.Validate({.Time = now}));
    EXPECT_FALSE(c.Validate({.Time = now.Add(2 * jwt::DefaultLeeway)}));

    // cannot be issued in the future
    EXPECT_EQ(c.Validate({.Time = now.Add(-2 * jwt::DefaultLeeway)}), jwt::ErrIssuedInTheFuture);
    EXPECT_EQ(c.Validate({.Time = now.Add(-jwt::DefaultLeeway - time::Nanosecond)}), jwt::ErrIssuedInTheFuture);

    // valid when we are within the default leeway
    EXPECT_FALSE(c.Validate({.Time = now.Add(-jwt::DefaultLeeway)}));

    // no leeway: valid up to the exact time; expired if issued one ns in the future
    EXPECT_FALSE(c.ValidateWithLeeway({.Time = now}, time::Duration()));
    EXPECT_EQ(c.ValidateWithLeeway({.Time = now.Add(-time::Nanosecond)}, time::Duration()), jwt::ErrIssuedInTheFuture);
}

TEST(GoJWT, OptionalDateClaims) {
    time::Time epoch;
    auto claimsWith = [](auto set) {
        jwt::Claims c;
        set(c);
        return c;
    };
    struct {
        const char* name;
        jwt::Claims claim;
        error want;
    } testCases[] = {
        {"no claims", jwt::Claims{}, nullptr},
        {"fail nbf",
         claimsWith([](jwt::Claims& c) { c.NotBefore = jwt::NewNumericDate(time::Now().Add(2 * time::Minute)); }),
         jwt::ErrNotValidYet},
        {"fail exp",
         claimsWith([&](jwt::Claims& c) { c.Expiry = jwt::NewNumericDate(epoch.Add(-7 * 24 * time::Hour)); }),
         jwt::ErrExpired},
        {"fail iat",
         claimsWith([](jwt::Claims& c) { c.IssuedAt = jwt::NewNumericDate(time::Now().Add(2 * time::Minute)); }),
         jwt::ErrIssuedInTheFuture},
    };
    for (const auto& tc : testCases) {
        EXPECT_EQ(tc.claim.Validate({.Time = epoch.Add(-24 * time::Hour)}), tc.want) << tc.name;
        // test with zero expected time: should not change anything
        EXPECT_EQ(tc.claim.Validate({}), tc.want) << tc.name;
    }
}

// ---------------------------------------------------------------------------
// claims_test.go

TEST(GoJWT, EncodeClaims) {
    auto now = date2016();
    jwt::Claims c;
    c.Issuer = "issuer";
    c.Subject = "subject";
    c.Audience = jwt::Audience{"a1", "a2"};
    c.NotBefore = jwt::NewNumericDate(time::Time{});
    c.IssuedAt = jwt::NewNumericDate(now);
    c.Expiry = jwt::NewNumericDate(now.Add(time::Hour));

    auto [b, err] = json::Marshal(c);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(asStringView(b), R"({"iss":"issuer","sub":"subject","aud":["a1","a2"],"exp":1451610000,"iat":1451606400})");
}

TEST(GoJWT, EncodeClaimsWithSingleAudience) {
    auto now = date2016();
    jwt::Claims c;
    c.Issuer = "issuer";
    c.Subject = "subject";
    c.Audience = jwt::Audience{"a1"};
    c.NotBefore = jwt::NewNumericDate(time::Time{});
    c.IssuedAt = jwt::NewNumericDate(now);
    c.Expiry = jwt::NewNumericDate(now.Add(time::Hour));

    auto [b, err] = json::Marshal(c);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(asStringView(b), R"({"iss":"issuer","sub":"subject","aud":"a1","exp":1451610000,"iat":1451606400})");
}

TEST(GoJWT, DecodeClaims) {
    auto now = date2016();
    jwt::Claims c;
    ASSERT_FALSE(json::Unmarshal(
        R"({"iss":"issuer","sub":"subject","aud":["a1","a2"],"exp":1451610000,"iat":1451606400})", &c));
    EXPECT_EQ(c.Issuer, "issuer");
    EXPECT_EQ(c.Subject, "subject");
    EXPECT_EQ(vec(c.Audience), (std::vector<std::string>{"a1", "a2"}));
    ASSERT_TRUE(c.IssuedAt && c.Expiry);
    EXPECT_TRUE(now.Equal(c.IssuedAt->Time()));
    EXPECT_TRUE(now.Add(time::Hour).Equal(c.Expiry->Time()));

    jwt::Claims c2;
    ASSERT_FALSE(json::Unmarshal(R"({"aud": "a1"})", &c2));
    EXPECT_EQ(vec(c2.Audience), (std::vector<std::string>{"a1"}));

    std::pair<std::string_view, error> invalid[] = {
        {R"({"aud": 5})", jwt::ErrUnmarshalAudience},
        {R"({"aud": ["foo", 5, "bar"]})", jwt::ErrUnmarshalAudience},
        {R"({"aud": null})", jwt::ErrUnmarshalAudience},
        {R"({"exp": "invalid"})", jwt::ErrUnmarshalNumericDate},
    };
    for (const auto& [raw, want] : invalid) {
        jwt::Claims c3;
        EXPECT_EQ(json::Unmarshal(raw, &c3), want) << raw;
    }
}

TEST(GoJWT, NumericDate) {
    // Go's (*NumericDate)(nil).Time() is the zero time; here the date is null.
    EXPECT_FALSE(jwt::NewNumericDate(time::Time{}));

    auto nonZeroDate = jwt::NewNumericDate(time::Unix(0, 0));
    ASSERT_TRUE(nonZeroDate);
    EXPECT_TRUE(nonZeroDate->Time().Equal(time::Unix(0, 0)));
    EXPECT_EQ(nonZeroDate->Time().Unix(), 0);
}

TEST(GoJWT, NumericDateOutOfRange) {
    // Values large enough to overflow the int64 conversion or time.Unix wrap
    // to a bogus instant, so they must be rejected rather than stored.
    for (std::string_view raw : {"1e19", "-1e19", "9.3e18", "9223372036800000000", "1e300", "4611686018427387904",
                                 "-4611686018427387904", "1e400"}) {
        jwt::NumericDate nd;
        EXPECT_TRUE(errors::Is(nd.UnmarshalJSON(as_bytes(raw)), jwt::ErrUnmarshalNumericDate)) << raw;
    }
    // Not numbers. Go's ParseFloat takes NaN and Inf, but the conversion
    // of NaN to an integer is undefined in C++: NaN is out of range too.
    for (std::string_view raw : {"NaN", "Inf", "-Inf", "\"1\"", "", "true", "null"}) {
        jwt::NumericDate nd;
        EXPECT_TRUE(errors::Is(nd.UnmarshalJSON(as_bytes(raw)), jwt::ErrUnmarshalNumericDate)) << raw;
    }

    // In-range values, including fractional seconds, still parse.
    jwt::NumericDate nd;
    ASSERT_FALSE(nd.UnmarshalJSON(as_bytes("1451606400.5")));
    EXPECT_EQ(nd.seconds, 1451606400);
    // Toward zero, as Go converts.
    ASSERT_FALSE(nd.UnmarshalJSON(as_bytes("-1.9")));
    EXPECT_EQ(nd.seconds, -1);
    ASSERT_FALSE(nd.UnmarshalJSON(as_bytes("4.6e18")));
    EXPECT_EQ(nd.seconds, 4600000000000000000);
}

// An out-of-range "nbf" must fail to decode rather than wrap into the past.
TEST(GoJWT, OutOfRangeNbfNotBeforeCheck) {
    jwt::Claims c;
    EXPECT_TRUE(errors::Is(json::Unmarshal(R"({"nbf": 1e19})", &c), jwt::ErrUnmarshalNumericDate));
}

TEST(GoJWT, EncodeClaimsTimeValues) {
    auto now = date2016();
    jwt::Claims c;
    c.NotBefore = jwt::NewNumericDate(time::Time{});
    c.IssuedAt = jwt::NewNumericDate(time::Unix(0, 0));
    c.Expiry = jwt::NewNumericDate(now);

    auto [b, err] = json::Marshal(c);
    ASSERT_FALSE(err) << err.Error();
    EXPECT_EQ(asStringView(b), R"({"exp":1451606400,"iat":0})");

    jwt::Claims c2;
    ASSERT_FALSE(json::Unmarshal(b, &c2));
    EXPECT_FALSE(c2.NotBefore);
    ASSERT_TRUE(c2.IssuedAt && c2.Expiry);
    EXPECT_TRUE(c.IssuedAt->Time().Equal(c2.IssuedAt->Time()));
    EXPECT_TRUE(c.Expiry->Time().Equal(c2.Expiry->Time()));
}

// ---------------------------------------------------------------------------
// builder_test.go

TEST(GoJWT, IntegerAndFloatsNormalize) {
    numberClaims c{int64(1) << 60, 12345.6789};
    auto [normalized, err] = jwt::detail::normalize(c);
    ASSERT_FALSE(err) << err.Error();

    auto [ni, err2] = strconv::ParseInt(asStringView(normalized["int"].data));
    ASSERT_FALSE(err2);
    auto [nf, err3] = strconv::ParseFloat(asStringView(normalized["float"].data), 64);
    ASSERT_FALSE(err3);
    EXPECT_EQ(ni, c.Int) << "normalize failed to preserve int64";
    EXPECT_EQ(nf, c.Float) << "normalize failed to preserve float64";
}

TEST(GoJWT, BuilderCustomClaimsNonPointer) {
    auto [token, err] = jwt::Signed(rsaSigner()).Claims(testClaims{"foo"}).Serialize();
    ASSERT_FALSE(err) << "Error creating JWT: " << err.Error();

    auto [parsed, err2] = jwt::ParseSigned(token, Algs{jose::RS256});
    ASSERT_FALSE(err2) << "Error parsing JWT: " << err2.Error();

    testClaims out;
    ASSERT_FALSE(parsed->Claims(testPrivRSAKey1()->PublicKey, &out));
    EXPECT_EQ(out.Subject, "foo");
}

TEST(GoJWT, BuilderCustomClaimsPointer) {
    testClaims in{"foo"};
    auto [token, err] = jwt::Signed(rsaSigner()).Claims(&in).Serialize();
    ASSERT_FALSE(err) << "Error creating JWT: " << err.Error();

    auto [parsed, err2] = jwt::ParseSigned(token, Algs{jose::RS256});
    ASSERT_FALSE(err2) << "Error parsing JWT: " << err2.Error();

    testClaims out;
    ASSERT_FALSE(parsed->Claims(testPrivRSAKey1()->PublicKey, &out));
    EXPECT_EQ(out.Subject, "foo");
}

TEST(GoJWT, BuilderMergeClaims) {
    jwt::Claims sub;
    sub.Subject = "42";
    auto [token, err] = jwt::Signed(rsaSigner())
                            .Claims(&sub)
                            .Claims(std::map<string, json::Value>{{"Scopes", json::Value::array{"read:users"}}})
                            .Serialize();
    ASSERT_FALSE(err) << "Error creating JWT: " << err.Error();

    auto [parsed, err2] = jwt::ParseSigned(token, Algs{jose::RS256});
    ASSERT_FALSE(err2) << "Error parsing JWT: " << err2.Error();

    std::map<string, json::Value> out;
    ASSERT_FALSE(parsed->Claims(testPrivRSAKey1()->PublicKey, &out));
    std::map<string, json::Value> want = {{"sub", "42"}, {"Scopes", json::Value::array{"read:users"}}};
    EXPECT_TRUE(out == want);

    testClaims foo{"foo"};
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims("invalid-claims").Claims(&foo).Serialize().second, jwt::ErrInvalidClaims);
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims(string("invalid-claims")).Serialize().second, jwt::ErrInvalidClaims);
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims(42).Serialize().second, jwt::ErrInvalidClaims);
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims(std::vector<string>{"a"}).Serialize().second, jwt::ErrInvalidClaims);
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims(static_cast<testClaims*>(nullptr)).Serialize().second,
              jwt::ErrInvalidClaims);
    EXPECT_EQ(jwt::Signed(rsaSigner()).Claims(json::Value("x")).Serialize().second, jwt::ErrInvalidClaims);

    invalidMarshalClaims bad;
    auto [s, err3] = jwt::Signed(rsaSigner()).Claims(&bad).Serialize();
    EXPECT_TRUE(contains(err3, "failed marshaling invalid claims")) << err3.Error();
}

TEST(GoJWT, BuilderHeadersSigner) {
    for (const auto& key : {testPrivRSAKey1(), testPrivRSAKey2()}) {
        auto keyIDBytes = slice<byte>::make(20);
        ASSERT_FALSE(rand::Read(keyIDBytes).second);
        std::string keyID(hex::EncodeToString(keyIDBytes));

        auto jwk = make_ptr<jose::JSONWebKey>();
        jwk->KeyID = keyID;
        jwk->Algorithm = "RSA";
        jwk->Key = key;
        auto [signer, err] = jose::NewSigner({jose::RS256, jwk}, nullptr);
        ASSERT_FALSE(err) << err.Error();

        jwt::Claims c;
        c.Issuer = "foo";
        auto [token, err2] = jwt::Signed(signer).Claims(&c).Serialize();
        ASSERT_FALSE(err2) << "failed to create token: " << err2.Error();
        auto [jws, err3] = jose::ParseSignedCompact(token, Algs{jose::RS256});
        ASSERT_FALSE(err3) << "parse signed: " << err3.Error();
        ASSERT_EQ(jws->Signatures.size(), 1u);
        EXPECT_EQ(jws->Signatures[0].Header.KeyID, keyID);
    }
}

// ---------------------------------------------------------------------------
// jwt_test.go

TEST(GoJWT, GetClaimsWithoutVerification) {
    auto [tok, err] = jwt::ParseSigned(signedToken(), Algs{jose::RS256});
    ASSERT_FALSE(err) << "Error parsing signed token: " << err.Error();
    jwt::Claims c;
    customClaims c2;
    ASSERT_FALSE(tok->UnsafeClaimsWithoutVerification(&c, &c2));
    EXPECT_EQ(c.Subject, "subject");
    EXPECT_EQ(c.Issuer, "issuer");
    EXPECT_EQ(c2.Scopes, (std::vector<string>{"s1", "s2"}));

    // A token that wasn't parsed has no claims.
    jwt::JSONWebToken empty;
    EXPECT_TRUE(empty.UnsafeClaimsWithoutVerification(&c));
    EXPECT_TRUE(empty.Claims(testPrivRSAKey1()->PublicKey, &c));
}

TEST(GoJWT, DecodeTokenWithJWKS) {
    jose::JSONWebKeySet jwks;
    jose::JSONWebKey k;
    k.KeyID = "foobar";
    k.Key = testPrivRSAKey1()->Public();
    jwks.Keys = append(jwks.Keys, k);

    auto [tok, err] = jwt::ParseSigned(rsaSignedTokenWithKid, Algs{jose::RS256});
    ASSERT_FALSE(err) << "Error parsing signed token: " << err.Error();

    std::map<string, json::Value> cl;
    ASSERT_FALSE(tok->Claims(jwks, &cl));
    EXPECT_TRUE(cl == subjectIssuerScopes());
}

// A JWT whose kid matches none of the keys of the JWKS.
TEST(GoJWT, DecodeTokenWithMismatchedJWKSKID) {
    jose::JSONWebKeySet jwks;
    jose::JSONWebKey k;
    k.KeyID = "does-not-match";
    k.Key = testPrivRSAKey1()->Public();
    jwks.Keys = append(jwks.Keys, k);

    for (const char* token : {rsaSignedTokenWithKid, rsaSignedToken}) {
        auto [tok, err] = jwt::ParseSigned(token, Algs{jose::RS256});
        ASSERT_FALSE(err) << "Error parsing signed token: " << err.Error();
        std::map<string, json::Value> cl;
        error err2 = tok->Claims(jwks, &cl);
        EXPECT_TRUE(errors::Is(err2, jose::ErrJWKSKidNotFound)) << err2.Error();
        EXPECT_TRUE(cl.empty());
    }
}

TEST(GoJWT, DecodeToken) {
    auto [tok, err] = jwt::ParseSigned(signedToken(), Algs{jose::RS256});
    ASSERT_FALSE(err) << err.Error();
    jwt::Claims c;
    customClaims c2;
    ASSERT_FALSE(tok->Claims(testPrivRSAKey1()->PublicKey, &c, &c2));
    EXPECT_EQ(c.Subject, "subject");
    EXPECT_EQ(c.Issuer, "issuer");
    EXPECT_EQ(c2.Scopes, (std::vector<string>{"s1", "s2"}));
    EXPECT_EQ(tok->Claims(testPrivRSAKey2()->PublicKey).Error(), "go-jose/go-jose: error in cryptographic primitive");

    auto [tok2, err2] = jwt::ParseSigned(rsaSignedToken, Algs{jose::RS256});
    ASSERT_FALSE(err2) << err2.Error();
    std::map<string, json::Value> cm;
    ASSERT_FALSE(tok2->Claims(testPrivRSAKey1()->PublicKey, &cm));
    EXPECT_TRUE(cm == subjectIssuerScopes());
    EXPECT_EQ(tok2->Claims(testPrivRSAKey2()->PublicKey).Error(), "go-jose/go-jose: error in cryptographic primitive");

    // HMAC-signed: no key verifies it.
    auto [tok3, err3] = jwt::ParseSigned(invalidPayloadSignedToken, Algs{jose::HS256});
    ASSERT_FALSE(err3) << err3.Error();
    jwt::Claims c3;
    EXPECT_EQ(tok3->Claims(testPrivRSAKey1()->PublicKey, &c3), jose::ErrCryptoFailure);
    EXPECT_TRUE(tok3->UnsafeClaimsWithoutVerification(&c3)) << "Expected unmarshaling claims to fail.";

    auto [tok4, err4] = jwt::ParseSigned(invalidPartsSignedToken, Algs{jose::HS256});
    EXPECT_EQ(err4.Error(), "go-jose/go-jose: compact JWS format must have three parts");

    // The algorithm is checked first.
    auto [tok5, err5] = jwt::ParseSigned(rsaSignedToken, Algs{jose::ES256});
    EXPECT_NE(errors::As<jose::ErrUnexpectedSignatureAlgorithm>(err5), nullptr);
}

TEST(GoJWT, TamperedJWT) {
    // Go's tampers with an encrypted token; this is a signed one.
    jwt::Claims cl;
    cl.Subject = "foo";
    cl.Issuer = "bar";
    auto [raw, err] = jwt::Signed(rsaSigner()).Claims(cl).Serialize();
    ASSERT_FALSE(err);

    // Modify with valid base64 junk
    auto r = strings::Split(raw, ".");
    ASSERT_EQ(r.size(), 3u);
    r[1] = "b3RoZXJ0aGluZw";
    auto [tok, err2] = jwt::ParseSigned(strings::Join(r, "."), Algs{jose::RS256});
    ASSERT_FALSE(err2);
    jwt::Claims cl2;
    EXPECT_TRUE(tok->Claims(testPrivRSAKey1()->PublicKey, &cl2)) << "Claims() on invalid token should fail";
}

TEST(GoJWT, AudienceContains) {
    jwt::Audience a{"a", "b"};
    EXPECT_TRUE(a.Contains("a"));
    EXPECT_FALSE(a.Contains("c"));
    EXPECT_FALSE(jwt::Audience{}.Contains(""));
    // An empty audience is left out of the claims, as Go's empty slice.
    jwt::Claims c;
    c.ID = "x";
    EXPECT_EQ(asStringView(json::Marshal(c).first), R"({"jti":"x"})");
}

TEST(GoJWT, ClaimsCaseSensitive) {
    // As go-jose's json: a member matches a claim only by its exact name.
    std::map<string, json::Value> raw = {{"iss", "a"}, {"ISS", "b"}, {"Sub", "x"}, {"scopes", json::Value::array{"s"}},
                                         {"SCOPES", json::Value::array{"t"}}};
    auto [token, err] = jwt::Signed(rsaSigner()).Claims(raw).Serialize();
    ASSERT_FALSE(err) << err.Error();
    auto [tok, err2] = jwt::ParseSigned(token, Algs{jose::RS256});
    ASSERT_FALSE(err2) << err2.Error();

    jwt::Claims c;
    customClaims cc;
    ASSERT_FALSE(tok->Claims(testPrivRSAKey1()->PublicKey, &c, &cc));
    EXPECT_EQ(c.Issuer, "a");
    EXPECT_EQ(c.Subject, "");
    EXPECT_EQ(cc.Scopes, (std::vector<string>{"s"}));

    jwt::Claims u;
    customClaims uc;
    ASSERT_FALSE(tok->UnsafeClaimsWithoutVerification(&u, &uc));
    EXPECT_EQ(u.Issuer, "a");
    EXPECT_EQ(u.Subject, "");
    EXPECT_EQ(uc.Scopes, (std::vector<string>{"s"}));

    // Decoding Claims directly is Go v1's json, as go++'s json::Unmarshal.
    jwt::Claims v;
    ASSERT_FALSE(json::Unmarshal(R"({"Sub":"x"})", &v));
    EXPECT_EQ(v.Subject, "x");
}
