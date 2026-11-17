// Ported from github.com/coreos/go-oidc/v3 (v3.21.0; Apache 2.0, see
// x/oidc/LICENSE): verify_test.go, jwks_test.go, oidc_test.go and
// oidctest/oidctest_test.go, with the names of their tests: TestVerify,
// TestVerifyAudience, TestVerifySigningAlg, TestAccessTokenHash;
// TestRSAVerify, TestECDSAVerify, TestEdDSAVerify, TestMultipleKeysVerify,
// TestMismatchedKeyID, TestKeyVerifyContextCanceled, TestRotation,
// TestUpdateKeysSendsCacheControlNoCache, TestJWKParseWithIgnoredKeys,
// TestJWKParseWithUnsupportedKeys; TestAccessTokenVerification,
// TestNewProvider, TestIssuerMismatchError, TestProviderConfigJSON,
// TestGetClient, TestUserInfoEndpoint, TestVerifierAlg, TestCanceledContext;
// and oidctest's TestServer (GoOIDCTest.Server).
//
// Left out: TestDistributedClaims and TestDistClaimResolver (distributed
// claims are not ported), logout_test.go (logout tokens are not ported) and
// BenchmarkVerify.
//
// Adapted: TestRotation moves the key set's clock past the refetch interval
// (a go++ addition, see <gopp/oidc.hpp>) before it rotates the keys, as the
// key set otherwise doesn't fetch again so soon. TestKeyVerifyContextCanceled
// waits for the fetch it started before the test ends.
//
// Added (UnknownKidDoesNotRefetchPerRequest and on): the refetch interval,
// and what verification does with tokens and documents an attacker made.
#include "oidc_internal.hpp"

#include <gopp/context.hpp>
#include <gopp/crypto.hpp>
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/elliptic.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/jose.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/oauth2.hpp>
#include <gopp/oidc.hpp>
#include <gopp/oidc/oidctest.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace gopp;
namespace internal = gopp::oidc::internal;

namespace {

std::string S(const string& s) { return std::string(s.view()); }

ptr<context::Context> bg() { return context::Background(); }

std::string unixIn(time::Duration d) { return std::to_string(time::Now().Add(d).Unix()); }

// A test server, closed at the end of the test.
struct server {
    explicit server(ptr<http::Handler> h) : ts(httptest::NewServer(std::move(h))) {}
    explicit server(http::handlerFn f) : server(make_ptr<http::HandlerFunc>(std::move(f))) {}
    ~server() { ts->Close(); }
    std::string URL() const { return S(ts->URL); }
    ptr<httptest::Server> ts;
};

// signingKey: a key pair and the algorithm it signs with.
struct signingKey {
    string keyID;  // optional
    ptr<crypto::PrivateKey> priv;
    ptr<crypto::PublicKey> pub;
    std::string_view alg;

    // sign: a JWS of payload, with the key's "kid".
    std::string sign(std::string_view payload) const {
        auto privKey = make_ptr<jose::JSONWebKey>();
        privKey->Key = priv;
        privKey->Algorithm = string(alg);
        privKey->KeyID = keyID;
        auto [signer, err] = jose::NewSigner({alg, privKey}, nullptr);
        EXPECT_FALSE(err) << err;
        if (err) return "";
        auto [jws, err2] = signer->Sign(as_bytes(payload));
        EXPECT_FALSE(err2) << err2;
        if (err2) return "";
        auto [data, err3] = jws->CompactSerialize();
        EXPECT_FALSE(err3) << err3;
        return S(data);
    }

    jose::JSONWebKey jwk() const {
        jose::JSONWebKey k;
        k.Key = pub;
        k.Use = "sig";
        k.Algorithm = string(alg);
        k.KeyID = keyID;
        return k;
    }
};

signingKey newRSAKey() {
    auto [priv, err] = rsa::GenerateKey(nullptr, 1028);
    EXPECT_FALSE(err) << err;
    return {"", priv, priv->Public(), jose::RS256};
}

signingKey newECDSAKey() {
    auto [priv, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    EXPECT_FALSE(err) << err;
    return {"", priv, priv->Public(), jose::ES256};
}

signingKey newEdDSAKey() {
    auto [pub, priv, err] = ed25519::GenerateKey(nullptr);
    EXPECT_FALSE(err) << err;
    return {"", priv, pub, jose::EdDSA};
}

ptr<jose::JSONWebSignature> parseSigned(std::string_view token) {
    auto [jws, err] = jose::ParseSigned(token, internal::allAlgs);
    EXPECT_FALSE(err) << err;
    return jws;
}

// keyServer: serves a key set, which a test may change.
struct keyServer final : http::Handler {
    sync::Mutex mu;
    jose::JSONWebKeySet keys;
    std::function<void(http::Header&)> setHeaders;

    void setKeys(std::vector<jose::JSONWebKey> ks) {
        std::lock_guard lock(mu);
        keys.Keys = slice<jose::JSONWebKey>();
        for (auto& k : ks) keys.Keys = append(keys.Keys, k);
    }

    void ServeHTTP(http::ResponseWriter& w, http::Request&) override {
        if (setHeaders) setHeaders(w.Header());
        std::lock_guard lock(mu);
        if (error err = json::Encode(w, keys)) panic(S(err.Error()));
    }
};

// A clock that a test moves forward.
struct testClock {
    std::shared_ptr<std::atomic<int64>> offset = std::make_shared<std::atomic<int64>>(0);
    std::function<time::Time()> fn() const {
        return [o = offset] { return time::Now().Add(time::Duration{o->load()}); };
    }
    void advance(time::Duration d) const { offset->fetch_add(d.ns); }
};

// ---------------------------------------------------------------------------
// verify_test.go

struct verificationTest {
    // Name of the subtest.
    std::string name{};
    // If not provided defaults to "https://foo".
    std::string issuer{};
    // JWT payload (just the claims).
    std::string idToken{};
    // Key to sign the ID Token with.
    std::optional<signingKey> signKey{};
    // If not provided defaults to signKey. Only useful when testing
    // invalid signatures.
    std::optional<signingKey> verificationKey{};
    oidc::Config config{};
    bool wantErr = false;
    bool wantErrExpiry = false;

    std::pair<ptr<oidc::IDToken>, error> runGetToken() const {
        std::string token;
        if (signKey) {
            token = signKey->sign(idToken);
        } else {
            // "none" still uses a second "." character, but "...MUST use the
            // empty octet sequence as its JWS Signature value."
            token = S(base64::RawURLEncoding.EncodeToString(as_bytes(R"({"alg": "none"})")));
            token += ".";
            token += S(base64::RawURLEncoding.EncodeToString(as_bytes(idToken)));
            token += ".";
        }
        std::string iss = issuer.empty() ? "https://foo" : issuer;
        ptr<oidc::KeySet> ks;
        if (verificationKey) {
            auto s = make_ptr<oidc::StaticKeySet>();
            s->PublicKeys = {verificationKey->pub};
            ks = s;
        } else if (signKey) {
            auto s = make_ptr<oidc::StaticKeySet>();
            s->PublicKeys = {signKey->pub};
            ks = s;
        }
        auto verifier = oidc::NewVerifier(iss, ks, config);
        return verifier->Verify(bg(), token);
    }

    void run() const {
        SCOPED_TRACE(name);
        auto [tok, err] = runGetToken();
        if (err && !wantErr && !wantErrExpiry) ADD_FAILURE() << err;
        if (!err && (wantErr || wantErrExpiry)) ADD_FAILURE() << "expected error";
        if (wantErrExpiry && !errors::As<oidc::TokenExpiredError>(err)) {
            ADD_FAILURE() << "expected *TokenExpiryError but got " << err;
        }
    }
};

oidc::Config skipChecks() { return {.SkipClientIDCheck = true, .SkipExpiryCheck = true}; }

}  // namespace

TEST(GoOIDC, Verify) {
    auto edCfg = skipChecks();
    edCfg.SupportedSigningAlgs = {string(oidc::EdDSA)};
    auto skipIss = skipChecks();
    skipIss.SkipIssuerCheck = true;
    auto insecure = skipChecks();
    insecure.InsecureSkipSignatureCheck = true;
    std::string hourAgo = unixIn(-time::Hour), inHour = unixIn(time::Hour);

    std::vector<verificationTest> tests = {
        {.name = "good token", .idToken = R"({"iss":"https://foo"})", .signKey = newRSAKey(), .config = skipChecks()},
        {.name = "good eddsa token", .idToken = R"({"iss":"https://foo"})", .signKey = newEdDSAKey(), .config = edCfg},
        {.name = "invalid issuer",
         .issuer = "https://bar",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newRSAKey(),
         .config = skipChecks(),
         .wantErr = true},
        {.name = "skip issuer check",
         .issuer = "https://bar",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newRSAKey(),
         .config = skipIss},
        {.name = "invalid sig",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newRSAKey(),
         .verificationKey = newRSAKey(),
         .config = skipChecks(),
         .wantErr = true},
        {.name = "google accounts without scheme",
         .issuer = "https://accounts.google.com",
         .idToken = R"({"iss":"accounts.google.com"})",
         .signKey = newRSAKey(),
         .config = skipChecks()},
        {.name = "expired token",
         .idToken = R"({"iss":"https://foo","exp":)" + hourAgo + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true},
         .wantErrExpiry = true},
        {.name = "unexpired token",
         .idToken = R"({"iss":"https://foo","exp":)" + inHour + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true}},
        {.name = "expiry as float",
         .idToken = R"({"iss":"https://foo","exp":)" +
                    std::format("{:E}", static_cast<double>(time::Now().Add(time::Hour).Unix())) + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true}},
        {.name = "nbf in future",
         .idToken = R"({"iss":"https://foo","nbf":)" + inHour + R"(,"exp":)" + inHour + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true},
         .wantErr = true},
        {.name = "nbf in past",
         .idToken = R"({"iss":"https://foo","nbf":)" + hourAgo + R"(,"exp":)" + inHour + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true}},
        {.name = "nbf in future within clock skew tolerance",
         .idToken = R"({"iss":"https://foo","nbf":)" + unixIn(30 * time::Second) + R"(,"exp":)" + inHour + "}",
         .signKey = newRSAKey(),
         .config = {.SkipClientIDCheck = true}},
        {.name = "unsigned token", .idToken = R"({"iss":"https://foo"})", .config = skipChecks(), .wantErr = true},
        {.name = "unsigned token InsecureSkipSignatureCheck", .idToken = R"({"iss":"https://foo"})", .config = insecure},
    };
    for (const auto& test : tests) test.run();
}

TEST(GoOIDC, VerifyAudience) {
    std::vector<verificationTest> tests = {
        {.name = "good audience",
         .idToken = R"({"iss":"https://foo","aud":"client1"})",
         .signKey = newRSAKey(),
         .config = {.ClientID = "client1", .SkipExpiryCheck = true}},
        {.name = "mismatched audience",
         .idToken = R"({"iss":"https://foo","aud":"client2"})",
         .signKey = newRSAKey(),
         .config = {.ClientID = "client1", .SkipExpiryCheck = true},
         .wantErr = true},
        {.name = "multiple audiences, one matches",
         .idToken = R"({"iss":"https://foo","aud":["client1","client2"]})",
         .signKey = newRSAKey(),
         .config = {.ClientID = "client2", .SkipExpiryCheck = true}},
    };
    for (const auto& test : tests) test.run();
}

TEST(GoOIDC, VerifySigningAlg) {
    auto algs = [](std::vector<std::string_view> as) {
        auto c = skipChecks();
        for (auto a : as) c.SupportedSigningAlgs = append(c.SupportedSigningAlgs, string(a));
        return c;
    };
    std::vector<verificationTest> tests = {
        {.name = "default signing alg", .idToken = R"({"iss":"https://foo"})", .signKey = newRSAKey(), .config = skipChecks()},
        {.name = "bad signing alg",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newECDSAKey(),
         .config = skipChecks(),
         .wantErr = true},
        {.name = "ecdsa signing", .idToken = R"({"iss":"https://foo"})", .signKey = newECDSAKey(), .config = algs({oidc::ES256})},
        {.name = "eddsa signing", .idToken = R"({"iss":"https://foo"})", .signKey = newEdDSAKey(), .config = algs({oidc::EdDSA})},
        {.name = "one of many supported",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newECDSAKey(),
         .config = algs({oidc::RS256, oidc::ES256})},
        {.name = "not in requiredAlgs",
         .idToken = R"({"iss":"https://foo"})",
         .signKey = newECDSAKey(),
         .config = algs({oidc::RS256, oidc::ES512}),
         .wantErr = true},
    };
    for (const auto& test : tests) test.run();
}

TEST(GoOIDC, AccessTokenHash) {
    std::string atHash = "piwt8oCH-K2D9pXlaS1Y-w";
    verificationTest vt{
        .name = "preserves token hash and sig algo",
        .idToken = R"({"iss":"https://foo","aud":"client1", "at_hash": ")" + atHash + R"("})",
        .signKey = newRSAKey(),
        .config = {.ClientID = "client1", .SkipExpiryCheck = true},
    };
    auto [tok, err] = vt.runGetToken();
    ASSERT_FALSE(err) << "parsing token: " << err;
    EXPECT_EQ(S(tok->AccessTokenHash), atHash) << "access token hash not preserved correctly";
    EXPECT_EQ(S(internal::idTokenAccess::sigAlgorithm(*tok)), oidc::RS256) << "invalid signature algo";
}

// ---------------------------------------------------------------------------
// jwks_test.go

namespace {

void testKeyVerify(const signingKey& good, const signingKey& bad, std::vector<signingKey> verification) {
    auto ks = make_ptr<keyServer>();
    std::vector<jose::JSONWebKey> keys;
    for (auto& v : verification) keys.push_back(v.jwk());
    ks->setKeys(keys);

    std::string payload = "a secret";
    auto jws = parseSigned(good.sign(payload));
    auto badJWS = parseSigned(bad.sign(payload));
    ASSERT_TRUE(jws && badJWS);

    server s(ks);
    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(bg(), s.URL());

    // Ensure the token verifies.
    auto [got, err] = internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(asStringView(got), payload);

    // Ensure the token verifies from the cache.
    auto [got2, err2] = internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
    ASSERT_FALSE(err2) << err2;
    EXPECT_EQ(asStringView(got2), payload);

    // Ensure item signed by wrong token doesn't verify.
    auto [got3, err3] = internal::remoteKeySetAccess::verify(*rks, bg(), *badJWS);
    EXPECT_TRUE(err3) << "incorrectly verified signature";
}

}  // namespace

TEST(GoOIDC, RSAVerify) {
    auto good = newRSAKey(), bad = newRSAKey();
    testKeyVerify(good, bad, {good});
}

TEST(GoOIDC, ECDSAVerify) {
    auto good = newECDSAKey(), bad = newECDSAKey();
    testKeyVerify(good, bad, {good});
}

TEST(GoOIDC, EdDSAVerify) {
    auto good = newEdDSAKey(), bad = newEdDSAKey();
    testKeyVerify(good, bad, {good});
}

TEST(GoOIDC, MultipleKeysVerify) {
    auto key1 = newRSAKey(), key2 = newRSAKey();
    auto bad = newECDSAKey();
    key1.keyID = "key1";
    key2.keyID = "key2";
    bad.keyID = "key3";
    testKeyVerify(key2, bad, {key1, key2});
}

TEST(GoOIDC, MismatchedKeyID) {
    auto key1 = newRSAKey(), key2 = newRSAKey();
    // The bad key is a valid key this time, but has a different Key ID. It
    // shouldn't match key1 because of the mismatched ID, even though it
    // would confirm the signature just fine.
    auto bad = key1;
    bad.keyID = "key3";
    key1.keyID = "key1";
    key2.keyID = "key2";
    testKeyVerify(key2, bad, {key1, key2});
}

TEST(GoOIDC, KeyVerifyContextCanceled) {
    auto [ctx, cancel] = context::WithCancel(bg());
    auto good = newECDSAKey();
    auto jws = parseSigned(good.sign("a secret"));
    ASSERT_TRUE(jws);

    server s([](http::ResponseWriter& w, http::Request&) { io::WriteString(w, "{}"); });
    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(ctx, s.URL());

    cancel();

    auto [got, err] = internal::remoteKeySetAccess::verify(*rks, ctx, *jws);
    ASSERT_TRUE(err) << "expected context canceled, got nil error";
    EXPECT_TRUE(errors::Is(err, context::Canceled)) << "expected error to be context canceled, got " << err;

    // go++: wait for the fetch, which goes on without the context.
    internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
}

TEST(GoOIDC, Rotation) {
    auto key1 = newRSAKey(), key2 = newRSAKey();
    key1.keyID = "key1";
    key2.keyID = "key2";

    std::string payload = "a secret";
    auto jws1 = parseSigned(key1.sign(payload));
    auto jws2 = parseSigned(key2.sign(payload));
    ASSERT_TRUE(jws1 && jws2);

    int cacheForSeconds = 1200;
    auto ks = make_ptr<keyServer>();
    ks->setKeys({key1.jwk()});
    ks->setHeaders = [&](http::Header& h) { h.Set("Cache-Control", string("max-age=" + std::to_string(cacheForSeconds))); };
    server s(ks);

    testClock clock;
    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(bg(), s.URL(), clock.fn());
    auto verify = [&](const jose::JSONWebSignature& jws) { return internal::remoteKeySetAccess::verify(*rks, bg(), jws).second; };

    EXPECT_FALSE(verify(*jws1)) << "failed to verify valid signature";
    EXPECT_TRUE(verify(*jws2)) << "incorrectly verified signature";

    // Add second key to public list.
    ks->setKeys({key1.jwk(), key2.jwk()});
    // go++: after the refetch interval.
    clock.advance(oidc::RemoteKeySetRefetchInterval);

    EXPECT_FALSE(verify(*jws1)) << "failed to verify valid signature";
    EXPECT_FALSE(verify(*jws2)) << "failed to verify valid signature";

    // Kill server. Keys should still be cached.
    s.ts->Close();

    EXPECT_FALSE(verify(*jws1)) << "failed to verify valid signature";
    EXPECT_FALSE(verify(*jws2)) << "failed to verify valid signature";
}

TEST(GoOIDC, UpdateKeysSendsCacheControlNoCache) {
    auto key = newRSAKey();
    sync::Mutex mu;
    std::vector<std::string> cacheControl;

    server s([&](http::ResponseWriter& w, http::Request& r) {
        {
            std::lock_guard lock(mu);
            cacheControl.push_back(S(r.Header.Get("Cache-Control")));
        }
        jose::JSONWebKeySet set;
        set.Keys = append(set.Keys, key.jwk());
        if (error err = json::Encode(w, set)) panic(S(err.Error()));
    });

    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(bg(), s.URL());
    auto jws = parseSigned(key.sign("a secret"));
    ASSERT_TRUE(jws);
    auto [got, err] = internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
    ASSERT_FALSE(err) << "failed to verify: " << err;

    std::lock_guard lock(mu);
    ASSERT_FALSE(cacheControl.empty()) << "expected at least one request to the JWKS endpoint";
    for (size_t i = 0; i < cacheControl.size(); i++) {
        EXPECT_EQ(cacheControl[i], "no-cache") << "request " << i;
    }
}

namespace {

// The JWKS test of TestJWKParseWithIgnoredKeys and ...UnsupportedKeys: an
// ES256 key, and others that must be skipped.
void testJWKParseWith(const std::string& otherKeys) {
    auto [priv, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(err) << err;
    jose::JSONWebKey pubKey;
    pubKey.Key = priv->Public();
    pubKey.Use = "sig";
    pubKey.Algorithm = string(jose::ES256);
    pubKey.KeyID = "key1";
    auto [pubRaw, err2] = json::Marshal(pubKey);
    ASSERT_FALSE(err2) << err2;
    auto privKey = make_ptr<jose::JSONWebKey>();
    privKey->Key = priv;
    privKey->Use = "sig";
    privKey->Algorithm = string(jose::ES256);
    privKey->KeyID = "key1";

    std::string body = R"({"keys": [)" + std::string(asStringView(pubRaw)) + "," + otherKeys + "]}";
    server s([&](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Content-Type", "application/json");
        io::WriteString(w, body);
    });
    auto ks = oidc::NewRemoteKeySet(bg(), s.URL());

    auto [signer, err3] = jose::NewSigner({jose::ES256, privKey}, nullptr);
    ASSERT_FALSE(err3) << err3;
    auto [jws, err4] = signer->Sign(as_bytes("payload"));
    ASSERT_FALSE(err4) << err4;
    auto [serialized, err5] = jws->CompactSerialize();
    ASSERT_FALSE(err5) << err5;

    auto [got, err6] = ks->VerifySignature(bg(), serialized.view());
    ASSERT_FALSE(err6) << "failed to verify signature: " << err6;
    EXPECT_EQ(asStringView(got), "payload");
}

}  // namespace

TEST(GoOIDC, JWKParseWithIgnoredKeys) {
    testJWKParseWith(R"({
        "alg": "ES256K",
        "kty": "EC",
        "x": "vsk5i5YJu8H_VPL7DWTgVGXBPrqgkyNmYvfgOrVut38",
        "y": "Mrg56tBhVeorHPXK1LbTX2jP7rEqOHIatM96HFzVMIU",
        "crv": "secp256k1",
        "kid": "oidc-es256k-1",
        "use": "sig"
    })");
}

TEST(GoOIDC, JWKParseWithUnsupportedKeys) {
    testJWKParseWith(R"({
        "kty": "OKP",
        "crv": "Ed448",
        "kid": "oidc-ed448-1",
        "use": "sig",
        "x": "gH1eRK-6hW6ZoAy2k11U4L5uaIaMaZTMCf1cAbsxsYLvTqV2-TQG1PNyLOrhZkMyzUJulMc1wAfH"
    },
    {
        "kty": "OKP",
        "crv": "X448",
        "kid": "oidc-x448-1",
        "use": "enc",
        "x": "5Tegc13rI3bLqrCiUo-Cg1Ijjaj0l1aZrkVfUFbp-B5CGSj5z3wdAr3B0K7pPYDm9qbtjmMKOf0"
    })");
}

// ---------------------------------------------------------------------------
// oidc_test.go

namespace {

// at_hash value and access_token returned by Google.
const std::string googleAccessTokenHash = "piwt8oCH-K2D9pXlaS1Y-w";
const std::string googleAccessToken = "ya29.CjHSA1l5WUn8xZ6HanHFzzdHdbXm-14rxnC7JHch9eFIsZkQEGoWzaYG4o7k5f6BnPLj";
constexpr std::string_view googleSigningAlg = oidc::RS256;
// following values computed by own algo for regression testing
const std::string computed384TokenHash = "_ILKVQjbEzFKNJjUKC2kz9eReYi0A9Of";
const std::string computed512TokenHash = "Spa_APgwBrarSeQbxI-rbragXho6dqFpH5x9PqaPfUI";

}  // namespace

TEST(GoOIDC, AccessTokenVerification) {
    auto newToken = [](std::string_view alg, std::string_view atHash) {
        oidc::IDToken t;
        internal::idTokenAccess::sigAlgorithm(t) = string(alg);
        t.AccessTokenHash = string(atHash);
        return t;
    };
    struct accessTokenTest {
        std::string name;
        oidc::IDToken tok;
        std::string accessToken;
        std::optional<std::string> wantMsg;  // nullopt: no error
    };
    std::vector<accessTokenTest> tests = {
        {"goodRS256", newToken(googleSigningAlg, googleAccessTokenHash), googleAccessToken, std::nullopt},
        {"goodES384", newToken("ES384", computed384TokenHash), googleAccessToken, std::nullopt},
        {"goodPS512", newToken("PS512", computed512TokenHash), googleAccessToken, std::nullopt},
        {"badRS256", newToken("RS256", computed512TokenHash), googleAccessToken,
         "access token hash does not match value in ID token"},
        {"nohash", newToken("RS256", ""), googleAccessToken, "id token did not have an access token hash"},
        {"EdDSA", newToken("EdDSA", computed512TokenHash), googleAccessToken, std::nullopt},
        {"badSignAlgo", newToken("none", "xxx"), googleAccessToken, R"(oidc: unsupported signing algorithm "none")"},
    };
    for (const auto& test : tests) {
        SCOPED_TRACE(test.name);
        error err = test.tok.VerifyAccessToken(test.accessToken);
        if (!test.wantMsg) {
            EXPECT_FALSE(err) << "want nil error, got " << err;
        } else {
            ASSERT_TRUE(err) << "expected error, got success";
            EXPECT_EQ(S(err.Error()), *test.wantMsg);
        }
    }
}

TEST(GoOIDC, NewProvider) {
    const std::string googleDoc = R"({
 "issuer": "ISSUER",
 "authorization_endpoint": "https://accounts.google.com/o/oauth2/v2/auth",
 "device_authorization_endpoint": "https://oauth2.googleapis.com/device/code",
 "token_endpoint": "https://oauth2.googleapis.com/token",
 "userinfo_endpoint": "https://openidconnect.googleapis.com/v1/userinfo",
 "revocation_endpoint": "https://oauth2.googleapis.com/revoke",
 "jwks_uri": "https://www.googleapis.com/oauth2/v3/certs",
 "response_types_supported": ["code", "token", "id_token", "code token", "code id_token", "token id_token",
  "code token id_token", "none"],
 "subject_types_supported": ["public"],
 "id_token_signing_alg_values_supported": ["RS256"],
 "scopes_supported": ["openid", "email", "profile"],
 "token_endpoint_auth_methods_supported": ["client_secret_post", "client_secret_basic"],
 "claims_supported": ["aud", "email", "email_verified", "exp", "family_name", "given_name", "iat", "iss",
  "locale", "name", "picture", "sub"],
 "code_challenge_methods_supported": ["plain", "S256"],
 "grant_types_supported": ["authorization_code", "refresh_token", "urn:ietf:params:oauth:grant-type:device_code",
  "urn:ietf:params:oauth:grant-type:jwt-bearer"]
})";
    auto doc = [](std::string_view issuer, std::string_view algs) {
        return R"({
            "issuer": ")" + std::string(issuer) + R"(",
            "authorization_endpoint": "https://example.com/auth",
            "token_endpoint": "https://example.com/token",
            "jwks_uri": "https://example.com/keys",
            "id_token_signing_alg_values_supported": )" + std::string(algs) + "}";
    };
    struct test {
        std::string name{}, data{}, issuerURLOverride{};
        bool trailingSlash = false;
        std::string wantAuthURL{}, wantTokenURL{}, wantDeviceAuthURL{}, wantUserInfoURL{}, wantIssuerURL{};
        std::vector<std::string> wantAlgorithms{};
        bool wantErr = false;
    };
    std::vector<test> tests = {
        {.name = "basic_case",
         .data = doc("ISSUER", R"(["RS256"])"),
         .wantAuthURL = "https://example.com/auth",
         .wantTokenURL = "https://example.com/token",
         .wantAlgorithms = {"RS256"}},
        {.name = "additional_algorithms",
         .data = doc("ISSUER", R"(["RS256", "RS384", "ES256", "EdDSA"])"),
         .wantAuthURL = "https://example.com/auth",
         .wantTokenURL = "https://example.com/token",
         .wantAlgorithms = {"RS256", "RS384", "ES256", "EdDSA"}},
        {.name = "unsupported_algorithms",
         .data = doc("ISSUER", R"(["RS256", "RS384", "ES256", "HS256", "none"])"),
         .wantAuthURL = "https://example.com/auth",
         .wantTokenURL = "https://example.com/token",
         .wantAlgorithms = {"RS256", "RS384", "ES256"}},
        {.name = "mismatched_issuer", .data = doc("https://example.com", R"(["RS256"])"), .wantErr = true},
        {.name = "mismatched_issuer_discovery_override",
         .data = doc("ISSUER", R"(["RS256"])"),
         .issuerURLOverride = "https://example.com",
         .wantAuthURL = "https://example.com/auth",
         .wantTokenURL = "https://example.com/token",
         .wantIssuerURL = "https://example.com",
         .wantAlgorithms = {"RS256"}},
        {.name = "issuer_with_trailing_slash",
         .data = doc("ISSUER", R"(["RS256"])"),
         .trailingSlash = true,
         .wantAuthURL = "https://example.com/auth",
         .wantTokenURL = "https://example.com/token",
         .wantAlgorithms = {"RS256"}},
        // Test case taken directly from:
        // https://accounts.google.com/.well-known/openid-configuration
        {.name = "google",
         .data = googleDoc,
         .wantAuthURL = "https://accounts.google.com/o/oauth2/v2/auth",
         .wantTokenURL = "https://oauth2.googleapis.com/token",
         .wantDeviceAuthURL = "https://oauth2.googleapis.com/device/code",
         .wantUserInfoURL = "https://openidconnect.googleapis.com/v1/userinfo",
         .wantAlgorithms = {"RS256"}},
    };
    for (const auto& test : tests) {
        SCOPED_TRACE(test.name);
        std::string issuer;
        server s([&](http::ResponseWriter& w, http::Request& r) {
            if (r.URL.Path != "/.well-known/openid-configuration") {
                http::NotFound(w, r);
                return;
            }
            w.Header().Set("Content-Type", "application/json");
            io::WriteString(w, S(strings::ReplaceAll(test.data, "ISSUER", issuer)));
        });
        issuer = s.URL();
        if (test.trailingSlash) issuer += "/";

        auto ctx = bg();
        if (!test.issuerURLOverride.empty()) ctx = oidc::InsecureIssuerURLContext(ctx, test.issuerURLOverride);

        auto [p, err] = oidc::NewProvider(ctx, issuer);
        if (err) {
            if (!test.wantErr) ADD_FAILURE() << "NewProvider() failed: " << err;
            continue;
        }
        if (test.wantErr) {
            ADD_FAILURE() << "NewProvider(): expected error";
            continue;
        }
        if (!test.wantIssuerURL.empty()) {
            EXPECT_EQ(S(internal::providerAccess::issuer(*p)), test.wantIssuerURL);
        }
        auto ep = p->Endpoint();
        EXPECT_EQ(S(ep.AuthURL), test.wantAuthURL);
        EXPECT_EQ(S(ep.TokenURL), test.wantTokenURL);
        EXPECT_EQ(S(ep.DeviceAuthURL), test.wantDeviceAuthURL);
        EXPECT_EQ(S(p->UserInfoEndpoint()), test.wantUserInfoURL);
        std::vector<std::string> algs;
        for (auto& a : internal::providerAccess::algorithms(*p)) algs.push_back(S(a));
        EXPECT_EQ(algs, test.wantAlgorithms);
    }
}

TEST(GoOIDC, IssuerMismatchError) {
    std::string issuer;
    server s([&](http::ResponseWriter& w, http::Request& r) {
        if (r.URL.Path != "/.well-known/openid-configuration") {
            http::NotFound(w, r);
            return;
        }
        w.Header().Set("Content-Type", "application/json");
        // The discovery document advertises an issuer that differs from the
        // one requested (note the trailing slash), which must be rejected.
        io::WriteString(w, R"({
            "issuer": ")" + issuer + R"(/",
            "authorization_endpoint": "https://example.com/auth",
            "token_endpoint": "https://example.com/token",
            "jwks_uri": "https://example.com/keys",
            "id_token_signing_alg_values_supported": ["RS256"]
        })");
    });
    issuer = s.URL();

    auto [p, err] = oidc::NewProvider(bg(), issuer);
    ASSERT_TRUE(err) << "NewProvider(): expected error, got nil";
    auto* mismatchErr = errors::As<oidc::IssuerMismatchError>(err);
    ASSERT_TRUE(mismatchErr) << "NewProvider() returned " << err << ", want *IssuerMismatchError";
    EXPECT_EQ(S(mismatchErr->Provided), issuer);
    EXPECT_EQ(S(mismatchErr->Discovered), issuer + "/");
}

TEST(GoOIDC, ProviderConfigJSON) {
    // https://accounts.google.com/.well-known/openid-configuration
    std::string testCase = R"(
{
 "issuer": "https://accounts.google.com",
 "authorization_endpoint": "https://accounts.google.com/o/oauth2/v2/auth",
 "device_authorization_endpoint": "https://oauth2.googleapis.com/device/code",
 "token_endpoint": "https://oauth2.googleapis.com/token",
 "userinfo_endpoint": "https://openidconnect.googleapis.com/v1/userinfo",
 "revocation_endpoint": "https://oauth2.googleapis.com/revoke",
 "jwks_uri": "https://www.googleapis.com/oauth2/v3/certs",
 "response_types_supported": ["code", "token", "id_token", "code token", "code id_token", "token id_token",
  "code token id_token", "none"],
 "subject_types_supported": ["public"],
 "id_token_signing_alg_values_supported": ["RS256"],
 "scopes_supported": ["openid", "email", "profile"],
 "token_endpoint_auth_methods_supported": ["client_secret_post", "client_secret_basic"],
 "claims_supported": ["aud", "email", "email_verified", "exp", "family_name", "given_name", "iat", "iss",
  "name", "picture", "sub"],
 "code_challenge_methods_supported": ["plain", "S256"],
 "grant_types_supported": ["authorization_code", "refresh_token", "urn:ietf:params:oauth:grant-type:device_code",
  "urn:ietf:params:oauth:grant-type:jwt-bearer"]
}
)";
    oidc::ProviderConfig config;
    ASSERT_FALSE(json::Unmarshal(testCase, &config)) << "Parsing provider config";
    EXPECT_EQ(S(config.IssuerURL), "https://accounts.google.com");
    EXPECT_EQ(S(config.AuthURL), "https://accounts.google.com/o/oauth2/v2/auth");
    EXPECT_EQ(S(config.TokenURL), "https://oauth2.googleapis.com/token");
    EXPECT_EQ(S(config.DeviceAuthURL), "https://oauth2.googleapis.com/device/code");
    EXPECT_EQ(S(config.UserInfoURL), "https://openidconnect.googleapis.com/v1/userinfo");
    EXPECT_EQ(S(config.JWKSURL), "https://www.googleapis.com/oauth2/v3/certs");
    ASSERT_EQ(config.Algorithms.size(), 1u);
    EXPECT_EQ(S(config.Algorithms[0]), "RS256");
}

TEST(GoOIDC, GetClient) {
    auto ctx = bg();
    EXPECT_FALSE(internal::getClient(ctx)) << "expected no http::Client from empty context";

    auto transport = make_ptr<http::Transport>();
    http::Client c{.Transport = transport, .Timeout = 5 * time::Second};
    ctx = oidc::ClientContext(ctx, c);
    auto got = internal::getClient(ctx);
    ASSERT_TRUE(got) << "expected http::Client from context";
    EXPECT_EQ(got->Transport.get(), transport.get());
    EXPECT_EQ(got->Timeout, 5 * time::Second);
    // The same key as x/oauth2's.
    EXPECT_TRUE(dynamic_pointer_cast<oauth2::ContextClient>(ctx->Value(oauth2::HTTPClient)));
}

namespace {

// testServer: a provider with a static key set, and a UserInfo endpoint if
// userInfo is set.
struct testServer {
    std::string contentType;
    std::string userInfo;
    ptr<httptest::Server> srv;

    ~testServer() {
        if (srv) srv->Close();
    }

    std::string run() {
        auto mux = make_ptr<http::ServeMux>();
        srv = httptest::NewServer(mux);
        std::string url = S(srv->URL);

        // generated using mkjwk.org
        std::string jwks = R"({
            "keys": [
                {
                    "kty": "RSA",
                    "e": "AQAB",
                    "use": "sig",
                    "kid": "test",
                    "alg": "RS256",
                    "n": "ilhCmTGFjjIPVN7Lfdn_fvpXOlzxa3eWnQGZ_eRa2ibFB1mnqoWxZJ8fkWIVFOQpsn66bIfWjBo_OI3sE6LhhRF8xhsMxlSeRKhpsWg0klYnMBeTWYET69YEAX_rGxy0MCZlFZ5tpr56EVZ-3QLfNiR4hcviqj9F2qE6jopfywsnlulJgyMi3N3kugit_JCNBJ0yz4ndZrMozVOtGqt35HhggUgYROzX6SWHUJdPXSmbAZU-SVLlesQhPfHS8LLq0sACb9OmdcwrpEFdbGCSTUPlHGkN5h6Zy8CS4s_bCdXKkjD20jv37M3GjRQkjE8vyMxFlo_qT8F8VZlSgXYTFw"
                }
            ]
        })";
        std::string userInfoJSON;
        if (!userInfo.empty()) userInfoJSON = R"("userinfo_endpoint": ")" + url + R"(/userinfo",)";
        std::string wellKnown = R"({
            "issuer": ")" + url + R"(",
            "authorization_endpoint": ")" + url + R"(/auth",
            "token_endpoint": ")" + url + R"(/token",
            "jwks_uri": ")" + url + R"(/keys",
            )" + userInfoJSON + R"(
            "id_token_signing_alg_values_supported": ["RS256"]
        })";

        mux->HandleFunc("/.well-known/openid-configuration",
                        [wellKnown](http::ResponseWriter& w, http::Request&) { io::WriteString(w, wellKnown); });
        mux->HandleFunc("/keys", [jwks](http::ResponseWriter& w, http::Request&) { io::WriteString(w, jwks); });
        if (!userInfo.empty()) {
            mux->HandleFunc("/userinfo", [ct = contentType, ui = userInfo](http::ResponseWriter& w, http::Request&) {
                w.Header().Add("Content-Type", string(ct));
                io::WriteString(w, ui);
            });
        }
        return url;
    }
};

}  // namespace

TEST(GoOIDC, UserInfoEndpoint) {
    std::string userInfoJSON = R"({
        "sub": "1234567890",
        "profile": "Joe Doe",
        "email": "joe@doe.com",
        "email_verified": true,
        "is_admin": true
    })";
    std::string userInfoJSONCognitoVariant = R"({
        "sub": "1234567890",
        "profile": "Joe Doe",
        "email": "joe@doe.com",
        "email_verified": "true",
        "is_admin": true
    })";
    // generated with jwt.io based on the private/public key pair
    std::string signedUserInfo =
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwicHJvZmlsZSI6IkpvZSBEb2UiLCJlbWFpbCI6Im"
        "pvZUBkb2UuY29tIiwiZW1haWxfdmVyaWZpZWQiOnRydWUsImlzX2FkbWluIjp0cnVlfQ.ejzc2IOLtvYp-2n5w3w4SW3rHNG9pOahnwpQ"
        "CwuIaj7DvO4SxDIzeJmFPMKTJUc-1zi5T42mS4Gs2r18KWhSkk8kqYermRX0VcGEEsH0r2BG5boeza_EjCoJ5-jBPX5ODWGhu2sZIkZl29"
        "IbaVSC8jk8qKnqacchiHNmuv_xXjRsAgUsqYftrEQOxqhpfL5KN2qtgeVTczg3ABqs2-SFeEzcgA1TnA9H3AynCPCVUMFgh7xyS8jxx7DN"
        "-1vRHBySz5gNbf8z8MNx_XBLfRxxxMF24rDIE8Z2gf1DEAPr4tT38hD8ugKSE84gC3xHJWFWsRLg-Ll6OQqshs82axS00Q";
    std::string signedUserInfoCognito =
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwicHJvZmlsZSI6IkpvZSBEb2UiLCJlbWFpbCI6Im"
        "pvZUBkb2UuY29tIiwiZW1haWxfdmVyaWZpZWQiOiJ0cnVlIiwiaXNfYWRtaW4iOnRydWV9.V9j6Q208fnj7E5dhCHnAktqndvelyz6PYx"
        "md2fLzA4ze8N770Tq9KFEE3QSM400GTxiP7tMyvBqnTj2q5Hr6DeRoy0WtLmYlnDfOJCr2qKbrPN0k94Ts9_sXAKEiJSKsTFUBHkrH4Nhy"
        "WsaBaPamI8ghuqPKJ1LniNuskHUlzBmDDW4mTy15ArsaIno8S4XVn19OoqODIO30axJJxKfxEbsDR3-YW4OD9qn80Wzw0zOsGJ04NJRfO5"
        "6VSprX0PhqvduOSUuHvm4cxtJIHHvj3AitrQriKZebZpXSs9PXPSPCysiQHyDz0A8y7R-sDgEhJlxe93nVbTU0itBehrbugQ";

    struct test {
        std::string name, contentType, userInfo;
        std::string wantEmail;
        bool wantEmailVerified = false;
    };
    std::vector<test> tests = {
        {"basic json userinfo", "application/json", userInfoJSON, "joe@doe.com", true},
        {"signed jwt userinfo", "application/jwt", signedUserInfo, "joe@doe.com", true},
        {"signed jwt userinfo, content-type with charset", "application/jwt; charset=ISO-8859-1", signedUserInfo,
         "joe@doe.com", true},
        {"basic json userinfo - cognito variant", "application/json", userInfoJSONCognitoVariant, "joe@doe.com", true},
        {"signed jwt userinfo - cognito variant", "application/jwt", signedUserInfoCognito, "joe@doe.com", true},
        {"no userinfo endpoint", "application/json", "", "", false},
    };
    for (const auto& test : tests) {
        SCOPED_TRACE(test.name);
        testServer ts{.contentType = test.contentType, .userInfo = test.userInfo, .srv = nullptr};
        std::string serverURL = ts.run();

        auto [provider, err] = oidc::NewProvider(bg(), serverURL);
        ASSERT_FALSE(err) << "Failed to initialize provider for test " << err;

        if (test.userInfo.empty()) {
            EXPECT_EQ(S(provider->UserInfoEndpoint()), "") << "expected UserInfoEndpoint to be empty";
            // provider.UserInfo will error.
            auto [info, err2] = provider->UserInfo(bg(), oauth2::StaticTokenSource(make_ptr<oauth2::Token>()));
            EXPECT_TRUE(err2);
            continue;
        }
        EXPECT_EQ(S(provider->UserInfoEndpoint()), serverURL + "/userinfo");

        auto [info, err2] = provider->UserInfo(bg(), oauth2::StaticTokenSource(make_ptr<oauth2::Token>()));
        ASSERT_FALSE(err2) << "failed to get userinfo " << err2;
        EXPECT_EQ(S(info->Email), test.wantEmail);
        EXPECT_EQ(info->EmailVerified, test.wantEmailVerified);
        EXPECT_EQ(S(info->Subject), "1234567890");
        EXPECT_EQ(S(info->Profile), "Joe Doe");
        struct adminClaims {
            bool is_admin = false;
        } claims;
        EXPECT_FALSE(info->Claims(&claims));
        EXPECT_TRUE(claims.is_admin);
    }
}

namespace {

// testIssuer: a provider of a discovery document and a key set.
struct testIssuer final : http::Handler {
    std::string baseURL;
    std::vector<std::string> algs;
    jose::JSONWebKeySet jwks;

    struct disc {
        [[= json::tag("issuer")]] std::string Issuer;
        [[= json::tag("jwks_uri")]] std::string JWKSURI;
        [[= json::tag("id_token_signing_alg_values_supported")]] std::vector<std::string> Algs;
    };

    void ServeHTTP(http::ResponseWriter& w, http::Request& r) override {
        if (r.URL.Path == "/.well-known/openid-configuration") {
            if (error err = json::Encode(w, disc{baseURL, baseURL + "/keys", algs})) panic(S(err.Error()));
        } else if (r.URL.Path == "/keys") {
            if (error err = json::Encode(w, jwks)) panic(S(err.Error()));
        } else {
            http::NotFound(w, r);
        }
    }
};

struct idClaims {
    [[= json::tag("iss")]] std::string Iss;
    [[= json::tag("sub")]] std::string Sub;
    [[= json::tag("aud")]] std::string Aud;
    [[= json::tag("exp")]] int64 Exp = 0;
    [[= json::tag("iat")]] int64 Iat = 0;
};

// A testIssuer of priv's public key, which signs with ES256.
ptr<testIssuer> newTestIssuer(const ptr<ecdsa::PrivateKey>& priv) {
    auto ts = make_ptr<testIssuer>();
    ts->algs = {"ES256"};
    jose::JSONWebKey pubKey;
    pubKey.Algorithm = "ES256";
    pubKey.Key = priv->Public();
    pubKey.Use = "sign";
    ts->jwks.Keys = append(ts->jwks.Keys, pubKey);
    return ts;
}

std::string signTestClaims(const ptr<ecdsa::PrivateKey>& priv, const std::string& iss) {
    auto now = time::Now();
    idClaims claims{iss, "test-user", "test-client", now.Add(time::Hour).Unix(), now.Add(-time::Hour).Unix()};
    auto [payload, err] = json::Marshal(claims);
    EXPECT_FALSE(err) << "marshaling claims: " << err;
    auto [signer, err2] = jose::NewSigner({jose::ES256, priv}, nullptr);
    EXPECT_FALSE(err2) << "creating signing key: " << err2;
    auto [jws, err3] = signer->Sign(payload);
    EXPECT_FALSE(err3) << "signing token: " << err3;
    auto [raw, err4] = jws->CompactSerialize();
    EXPECT_FALSE(err4) << "serializing token: " << err4;
    return S(raw);
}

}  // namespace

TEST(GoOIDC, VerifierAlg) {
    auto [priv, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(err) << "generating random key: " << err;
    auto ts = newTestIssuer(priv);
    server srv(ts);
    ts->baseURL = srv.URL();

    auto [provider, err2] = oidc::NewProvider(bg(), srv.URL());
    ASSERT_FALSE(err2) << "creating provider: " << err2;

    std::string rawIDToken = signTestClaims(priv, srv.URL());
    auto verifier = provider->Verifier({.ClientID = "test-client"});
    auto [idToken, err3] = verifier->Verify(bg(), rawIDToken);
    ASSERT_FALSE(err3) << "verifying token: " << err3;
    EXPECT_EQ(S(idToken->Subject), "test-user");
}

TEST(GoOIDC, CanceledContext) {
    auto [priv, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(err) << "generating random key: " << err;
    auto ts = newTestIssuer(priv);
    server srv(ts);
    ts->baseURL = srv.URL();

    auto [ctx, cancel] = context::WithCancel(bg());
    auto [provider, err2] = oidc::NewProvider(ctx, srv.URL());
    ASSERT_FALSE(err2) << "creating provider: " << err2;

    // Explicitly cancel the context.
    cancel();

    std::string rawIDToken = signTestClaims(priv, srv.URL());
    auto verifier = provider->Verifier({.ClientID = "test-client"});
    auto [idToken, err3] = verifier->Verify(bg(), rawIDToken);
    ASSERT_FALSE(err3) << "verifying token: " << err3;
}

// ---------------------------------------------------------------------------
// oidctest_test.go

TEST(GoOIDCTest, Server) {
    auto [priv, err] = rsa::GenerateKey(nullptr, 2048);
    ASSERT_FALSE(err) << "creating server: " << err;

    auto s = make_ptr<oidctest::Server>();
    s->PublicKeys = {{.PublicKey = priv->Public(), .KeyID = "my-key-id", .Algorithm = string(oidc::RS256)}};
    server srv(s);
    s->SetIssuer(srv.URL());

    auto now = time::Now();
    std::string rawClaims = R"({
        "iss": ")" + srv.URL() + R"(",
        "aud": "my-client-id",
        "sub": "foo",
        "exp": )" + std::to_string(now.Add(time::Hour).Unix()) + R"(,
        "email": "foo@example.com",
        "email_verified": true
    })";
    string token = oidctest::SignIDToken(priv, "my-key-id", oidc::RS256, rawClaims);

    auto [p, err2] = oidc::NewProvider(bg(), srv.URL());
    ASSERT_FALSE(err2) << "new provider: " << err2;
    auto v = p->VerifierContext(bg(), {.ClientID = "my-client-id", .Now = [now] { return now; }});

    auto [idToken, err3] = v->Verify(bg(), token.view());
    ASSERT_FALSE(err3) << "verifying token: " << err3;
    EXPECT_EQ(S(idToken->Subject), "foo");

    struct {
        string email;
        bool email_verified = false;
    } claims;
    ASSERT_FALSE(idToken->Claims(&claims)) << "parsing id token claims";
    EXPECT_EQ(S(claims.email), "foo@example.com");
    EXPECT_TRUE(claims.email_verified);
}

// ---------------------------------------------------------------------------
// Added: the refetch interval, and hostile input.

TEST(GoOIDC, UnknownKidDoesNotRefetchPerRequest) {
    auto key = newRSAKey();
    key.keyID = "k1";
    std::atomic<int> fetches{0};
    std::string set;
    {
        jose::JSONWebKeySet s;
        s.Keys = append(s.Keys, key.jwk());
        auto [b, err] = json::Marshal(s);
        ASSERT_FALSE(err) << err;
        set = std::string(asStringView(b));
    }
    server s([&](http::ResponseWriter& w, http::Request&) {
        fetches++;
        io::WriteString(w, set);
    });

    // 50 tokens of the right issuer, signed with the key, with made-up kids.
    constexpr int N = 50, fibers = 8;
    std::vector<std::string> tokens;
    for (int i = 0; i < N; i++) {
        auto k = key;
        k.keyID = string("unknown-" + std::to_string(i) + "-" + std::to_string(time::Now().UnixNano()));
        tokens.push_back(k.sign(R"({"iss":"https://foo","aud":"client"})"));
    }

    auto verifier = oidc::NewVerifier("https://foo", oidc::NewRemoteKeySet(bg(), s.URL()),
                                      {.ClientID = "client", .SkipExpiryCheck = true});
    std::atomic<int> next{0}, verified{0};
    sync::WaitGroup wg;
    wg.Add(fibers);
    for (int f = 0; f < fibers; f++) {
        go([&] {
            for (int i; (i = next++) < N;) {
                auto [tok, err] = verifier->Verify(bg(), tokens[i]);
                if (!err) verified++;
            }
            wg.Done();
        });
    }
    wg.Wait();

    EXPECT_EQ(verified.load(), 0) << "a token with an unknown kid verified";
    EXPECT_GE(fetches.load(), 1);
    EXPECT_LE(fetches.load(), 2) << "the key set was fetched for each unknown kid";

    // A token with the key's kid still verifies, from the cache.
    int before = fetches.load();
    auto [tok, err] = verifier->Verify(bg(), key.sign(R"({"iss":"https://foo","aud":"client"})"));
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(fetches.load(), before);
}

TEST(GoOIDC, RefetchAfterInterval) {
    auto key = newRSAKey();
    key.keyID = "k1";
    std::atomic<int> fetches{0};
    auto ks = make_ptr<keyServer>();
    ks->setKeys({key.jwk()});
    server s([&](http::ResponseWriter& w, http::Request& r) {
        fetches++;
        ks->ServeHTTP(w, r);
    });

    testClock clock;
    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(bg(), s.URL(), clock.fn());
    auto other = key;
    other.keyID = "k2";
    auto jws = parseSigned(other.sign("payload"));
    ASSERT_TRUE(jws);
    auto verify = [&] { return internal::remoteKeySetAccess::verify(*rks, bg(), *jws).second; };

    EXPECT_TRUE(verify());
    EXPECT_EQ(fetches.load(), 1);
    EXPECT_TRUE(verify());
    EXPECT_EQ(fetches.load(), 1) << "fetched within the refetch interval";
    clock.advance(oidc::RemoteKeySetRefetchInterval - time::Second);
    EXPECT_TRUE(verify());
    EXPECT_EQ(fetches.load(), 1) << "fetched within the refetch interval";

    // The provider rotates in the key: found once the interval has passed.
    ks->setKeys({key.jwk(), other.jwk()});
    clock.advance(time::Second);
    EXPECT_FALSE(verify());
    EXPECT_EQ(fetches.load(), 2);
}

TEST(GoOIDC, FailedFetchIsRateLimited) {
    std::atomic<int> fetches{0};
    server s([&](http::ResponseWriter& w, http::Request&) {
        fetches++;
        http::Error(w, "down", http::StatusInternalServerError);
    });
    auto key = newRSAKey();
    auto jws = parseSigned(key.sign("payload"));
    ASSERT_TRUE(jws);
    testClock clock;
    auto rks = internal::remoteKeySetAccess::newRemoteKeySet(bg(), s.URL(), clock.fn());
    for (int i = 0; i < 3; i++) {
        auto [got, err] = internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
        ASSERT_TRUE(err);
        EXPECT_NE(S(err.Error()).find("500"), std::string::npos) << err;
    }
    EXPECT_EQ(fetches.load(), 1);
    clock.advance(oidc::RemoteKeySetRefetchInterval);
    internal::remoteKeySetAccess::verify(*rks, bg(), *jws);
    EXPECT_EQ(fetches.load(), 2);
}

TEST(GoOIDC, SignatureCheckedFirst) {
    // A token that fails every check, with a bad signature: the error is the
    // signature's, not the expiry's or the issuer's.
    verificationTest vt{
        .name = "expired, wrong issuer and audience, bad signature",
        .issuer = "https://bar",
        .idToken = R"({"iss":"https://foo","aud":"other","exp":)" + unixIn(-time::Hour) + "}",
        .signKey = newRSAKey(),
        .verificationKey = newRSAKey(),
        .config = {.ClientID = "client"},
    };
    auto [tok, err] = vt.runGetToken();
    ASSERT_TRUE(err);
    EXPECT_FALSE(errors::As<oidc::TokenExpiredError>(err)) << err;
    EXPECT_NE(S(err.Error()).find("failed to verify signature"), std::string::npos) << err;
}

TEST(GoOIDC, VerifyChecks) {
    auto key = newRSAKey();
    auto now = time::Now();
    std::string exp = std::to_string(now.Unix());
    std::vector<verificationTest> tests = {
        // The audience is required unless SkipClientIDCheck.
        {.name = "no client ID", .idToken = R"({"iss":"https://foo","aud":"x"})", .signKey = key,
         .config = {.SkipExpiryCheck = true}, .wantErr = true},
        {.name = "no audience", .idToken = R"({"iss":"https://foo"})", .signKey = key,
         .config = {.ClientID = "client", .SkipExpiryCheck = true}, .wantErr = true},
        // Expiry uses the configured clock.
        {.name = "expired by Now", .idToken = R"({"iss":"https://foo","exp":)" + exp + "}", .signKey = key,
         .config = {.SkipClientIDCheck = true, .Now = [now] { return now.Add(time::Second); }}, .wantErrExpiry = true},
        {.name = "not expired by Now", .idToken = R"({"iss":"https://foo","exp":)" + exp + "}", .signKey = key,
         .config = {.SkipClientIDCheck = true, .Now = [now] { return now.Add(-time::Second); }}},
        {.name = "no exp", .idToken = R"({"iss":"https://foo"})", .signKey = key,
         .config = {.SkipClientIDCheck = true}, .wantErrExpiry = true},
        // The claims are case-sensitive, as go-jose's.
        {.name = "ISS is not iss", .idToken = R"({"ISS":"https://foo"})", .signKey = key, .config = skipChecks(),
         .wantErr = true},
        {.name = "AUD is not aud", .idToken = R"({"iss":"https://foo","AUD":"client"})", .signKey = key,
         .config = {.ClientID = "client", .SkipExpiryCheck = true}, .wantErr = true},
        {.name = "iss, then ISS", .idToken = R"({"iss":"https://foo","ISS":"https://bar"})", .signKey = key,
         .config = skipChecks()},
    };
    for (const auto& test : tests) test.run();

    // IDToken::Claims is case-sensitive too.
    verificationTest vt{.idToken = R"({"iss":"https://foo","Email":"a@b"})", .signKey = key, .config = skipChecks()};
    auto [tok, err] = vt.runGetToken();
    ASSERT_FALSE(err) << err;
    struct {
        string email;
    } claims;
    ASSERT_FALSE(tok->Claims(&claims));
    EXPECT_EQ(S(claims.email), "");
    json::Value v;
    ASSERT_FALSE(tok->Claims(&v));
    EXPECT_EQ(S(v["Email"].AsString()), "a@b");
}

TEST(GoOIDC, MalformedTokens) {
    auto key = newRSAKey();
    auto ks = make_ptr<oidc::StaticKeySet>();
    ks->PublicKeys = {key.pub};
    auto verifier = oidc::NewVerifier("https://foo", ks, skipChecks());
    auto b64 = [](std::string_view s) { return S(base64::RawURLEncoding.EncodeToString(as_bytes(s))); };
    std::string good = key.sign(R"({"iss":"https://foo"})");
    auto dot1 = good.find('.'), dot2 = good.rfind('.');
    std::string header = good.substr(0, dot1), sig = good.substr(dot2 + 1);

    std::vector<std::string> tokens = {
        "", ".", "..", "...", "a.b.c", "garbage", "eyJ.eyJ.", std::string(100000, '.'),
        b64(R"({"alg":"RS256"})") + "." + b64("{}") + ".",
        // The payload changed after signing.
        header + "." + b64(R"({"iss":"https://foo","sub":"admin"})") + "." + sig,
        // The signature of another token.
        key.sign(R"({"iss":"https://foo","x":1})").substr(0, good.size() - sig.size()) + sig,
        // Algorithms that never verify here: none, and HMAC (with the public
        // key as the secret, the classic confusion).
        b64(R"({"alg":"none"})") + "." + b64(R"({"iss":"https://foo"})") + ".",
        b64(R"({"alg":"HS256"})") + "." + b64(R"({"iss":"https://foo"})") + "." + b64("0123456789abcdef0123456789abcdef"),
        // A key in the header is not trusted.
        b64(R"({"alg":"RS256","jwk":{"kty":"RSA","n":"AQAB","e":"AQAB"}})") + "." + b64(R"({"iss":"https://foo"})") + "." + sig,
        // The JSON serialization.
        R"({"payload":"e30","protected":")" + header + R"(","signature":")" + sig + R"("})",
    };
    for (const auto& t : tokens) {
        auto [tok, err] = verifier->Verify(bg(), t);
        EXPECT_TRUE(err) << "verified: " << t.substr(0, 200);
    }
    // HS256 allowed by the config still never verifies with these keys.
    auto cfg = skipChecks();
    cfg.SupportedSigningAlgs = {"HS256", "none"};
    auto [tok, err] = oidc::NewVerifier("https://foo", ks, cfg)->Verify(bg(), tokens[12]);
    EXPECT_TRUE(err);
    auto [tok2, err2] = oidc::NewVerifier("https://foo", ks, cfg)->Verify(bg(), tokens[11]);
    EXPECT_TRUE(err2);

    // Payloads that are signed but aren't claims.
    for (std::string payload : std::vector<std::string>{"not json", "[]", "5", R"({"aud":5})", R"({"exp":"abc"})", R"({"exp":1e400})",
                                R"({"exp":-1e300})", R"({"exp":1e300})", R"({"iss":["a"]})", R"({"nbf":true})",
                                std::string(10000, '[') + std::string(10000, ']')}) {
        auto [tok, err] = verifier->Verify(bg(), key.sign(payload));
        EXPECT_TRUE(err) << payload.substr(0, 100);
    }
    // Times that don't fit are errors, not undefined or wrapped around.
    for (std::string exp : {"1e300", "-1e300", "9223372036854775807", "-9223372036854775808", "4611686018427387904"}) {
        auto [tok3, err3] = oidc::NewVerifier("https://foo", ks, {.SkipClientIDCheck = true})
                                ->Verify(bg(), key.sign(R"({"iss":"https://foo","exp":)" + exp + "}"));
        ASSERT_TRUE(err3) << exp;
        EXPECT_FALSE(errors::As<oidc::TokenExpiredError>(err3)) << err3;
    }
    auto [tok4, err4] = oidc::NewVerifier("https://foo", ks, {.SkipClientIDCheck = true})
                            ->Verify(bg(), key.sign(R"({"iss":"https://foo","exp":"4611686018427387903"})"));
    EXPECT_FALSE(err4) << err4;

    // No key set: no verification.
    auto [tok5, err5] = oidc::NewVerifier("https://foo", nullptr, skipChecks())->Verify(bg(), good);
    EXPECT_TRUE(err5);
    // A StaticKeySet with a null key, or none.
    auto bad = make_ptr<oidc::StaticKeySet>();
    bad->PublicKeys = {nullptr, key.pub};
    EXPECT_TRUE(oidc::NewVerifier("https://foo", bad, skipChecks())->Verify(bg(), good).second);
    EXPECT_TRUE(oidc::NewVerifier("https://foo", make_ptr<oidc::StaticKeySet>(), skipChecks())->Verify(bg(), good).second);
}

TEST(GoOIDC, HostileDocuments) {
    auto key = newRSAKey();
    std::string token = key.sign(R"({"iss":"https://foo"})");
    std::string deep = std::string(100000, '[') + std::string(100000, ']');
    std::string deepObj;
    for (int i = 0; i < 50000; i++) deepObj += R"({"a":)";
    deepObj += "1" + std::string(50000, '}');

    for (std::string doc : {std::string("garbage"), std::string("[]"), std::string("null"), std::string(R"({"keys":"x"})"),
                            std::string(R"({"keys":[1]})"), std::string(R"({"keys":[null]})"),
                            std::string(R"({"keys":[{"kty":"RSA","n":"AQAB","e":"AQAB"}]})"),
                            std::string(R"({"keys":[{"kty":"RSA","n":"","e":""}]})"),
                            std::string(R"({"keys":[{"kty":"EC","crv":"P-256","x":"AA","y":"AA"}]})"),
                            std::string(R"({"keys":[{"kty":"oct","alg":"HS256","k":"c2VjcmV0"}]})"),
                            std::string(R"({"keys":[{"kty":"OKP","crv":"Ed25519","x":"AA"}]})"),
                            std::string(R"({"keys":[{"alg":5}]})"), deep, deepObj}) {
        SCOPED_TRACE(doc.substr(0, 80));
        server s([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, doc); });
        auto [got, err] = oidc::NewRemoteKeySet(bg(), s.URL())->VerifySignature(bg(), token);
        EXPECT_TRUE(err);
    }

    for (std::string doc : {std::string("garbage"), std::string("[]"), std::string("null"), std::string(R"({"issuer":5})"),
                            std::string(R"({"issuer":"ISSUER","id_token_signing_alg_values_supported":"RS256"})"),
                            deep, deepObj}) {
        SCOPED_TRACE(doc.substr(0, 80));
        std::string issuer;
        server s([&](http::ResponseWriter& w, http::Request&) {
            io::WriteString(w, S(strings::ReplaceAll(doc, "ISSUER", issuer)));
        });
        issuer = s.URL();
        auto [p, err] = oidc::NewProvider(bg(), issuer);
        EXPECT_TRUE(err);
    }

    // A discovery document of an error status.
    server s([&](http::ResponseWriter& w, http::Request&) { http::Error(w, "nope", http::StatusNotFound); });
    auto [p, err] = oidc::NewProvider(bg(), s.URL());
    ASSERT_TRUE(err);
    EXPECT_NE(S(err.Error()).find("404"), std::string::npos) << err;
}

TEST(GoOIDC, ResponseSizeLimit) {
    auto key = newRSAKey();
    std::string set;
    {
        jose::JSONWebKeySet s;
        s.Keys = append(s.Keys, key.jwk());
        auto [b, err] = json::Marshal(s);
        ASSERT_FALSE(err) << err;
        set = std::string(asStringView(b));
    }
    std::string token = key.sign(R"({"iss":"https://foo"})");
    for (auto [size, ok] : {std::pair{oidc::MaxResponseSize, true}, std::pair{oidc::MaxResponseSize + 1, false}}) {
        SCOPED_TRACE(size);
        // The key set, padded with white space to size bytes.
        std::string body = set + std::string(static_cast<size_t>(size) - set.size(), ' ');
        server s([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, body); });
        auto [got, err] = oidc::NewRemoteKeySet(bg(), s.URL())->VerifySignature(bg(), token);
        EXPECT_EQ(!err, ok) << err;
    }

    std::string issuer;
    server s([&](http::ResponseWriter& w, http::Request&) {
        std::string doc = R"({"issuer":")" + issuer + R"("})";
        io::WriteString(w, doc + std::string(static_cast<size_t>(oidc::MaxResponseSize), ' '));
    });
    issuer = s.URL();
    auto [p, err] = oidc::NewProvider(bg(), issuer);
    ASSERT_TRUE(err);
    EXPECT_NE(S(err.Error()).find("too large"), std::string::npos) << err;
}

namespace {

struct countingTransport final : http::RoundTripper {
    std::atomic<int> n{0};
    sync::Mutex mu;
    std::vector<std::string> paths;
    std::pair<ptr<http::Response>, error> RoundTrip(http::Request& req) override {
        n++;
        {
            std::lock_guard lock(mu);
            paths.push_back(S(req.URL.Path));
        }
        return http::DefaultTransport()->RoundTrip(req);
    }
};

}  // namespace

TEST(GoOIDC, ContextClientIsUsed) {
    auto [priv, err] = rsa::GenerateKey(nullptr, 2048);
    ASSERT_FALSE(err) << err;
    auto op = make_ptr<oidctest::Server>();
    op->PublicKeys = {{.PublicKey = priv->Public(), .KeyID = "k1", .Algorithm = string(oidc::RS256)}};
    server srv([op](http::ResponseWriter& w, http::Request& r) {
        if (r.URL.Path == "/userinfo") {
            w.Header().Set("Content-Type", "application/json");
            io::WriteString(w, R"({"sub":"foo","email":"foo@example.com"})");
            return;
        }
        op->ServeHTTP(w, r);
    });
    op->SetIssuer(srv.URL());
    string token = oidctest::SignIDToken(priv, "k1", oidc::RS256,
                                         R"({"iss":")" + srv.URL() + R"(","aud":"c","exp":)" + unixIn(time::Hour) + "}");

    auto t1 = make_ptr<countingTransport>();
    auto ctx = oidc::ClientContext(bg(), http::Client{.Transport = t1});

    // Discovery.
    auto [p, err2] = oidc::NewProvider(ctx, srv.URL());
    ASSERT_FALSE(err2) << err2;
    EXPECT_EQ(t1->n.load(), 1);

    // The key set of Verifier: the client of NewProvider's context.
    auto [tok, err3] = p->Verifier({.ClientID = "c"})->Verify(bg(), token.view());
    ASSERT_FALSE(err3) << err3;
    EXPECT_EQ(t1->n.load(), 2);

    // The key set of VerifierContext: the client of its context.
    auto t2 = make_ptr<countingTransport>();
    auto [tok2, err4] = p->VerifierContext(oidc::ClientContext(bg(), http::Client{.Transport = t2}), {.ClientID = "c"})
                            ->Verify(bg(), token.view());
    ASSERT_FALSE(err4) << err4;
    EXPECT_EQ(t2->n.load(), 1);

    // UserInfo: the client of its context.
    oidc::ProviderConfig pc{.IssuerURL = string(srv.URL()), .UserInfoURL = string(srv.URL() + "/userinfo")};
    auto [info, err5] = pc.NewProvider(bg())->UserInfo(ctx, oauth2::StaticTokenSource(make_ptr<oauth2::Token>()));
    ASSERT_FALSE(err5) << err5;
    EXPECT_EQ(S(info->Email), "foo@example.com");
    EXPECT_EQ(t1->n.load(), 3);

    std::lock_guard lock(t1->mu);
    EXPECT_EQ(t1->paths, (std::vector<std::string>{"/.well-known/openid-configuration", "/keys", "/userinfo"}));
}

TEST(GoOIDC, UserInfoSignedWithProviderKeys) {
    // A signed UserInfo response must verify with the provider's keys.
    auto [priv, err] = rsa::GenerateKey(nullptr, 2048);
    ASSERT_FALSE(err) << err;
    auto [other, err2] = rsa::GenerateKey(nullptr, 2048);
    ASSERT_FALSE(err2) << err2;
    auto op = make_ptr<oidctest::Server>();
    op->PublicKeys = {{.PublicKey = priv->Public(), .KeyID = "k1", .Algorithm = string(oidc::RS256)}};
    string forged = oidctest::SignIDToken(other, "k1", oidc::RS256, R"({"sub":"admin","email":"admin@example.com"})");
    server srv([op, forged](http::ResponseWriter& w, http::Request& r) {
        if (r.URL.Path == "/userinfo") {
            w.Header().Set("Content-Type", "application/jwt");
            io::WriteString(w, forged.view());
            return;
        }
        op->ServeHTTP(w, r);
    });
    op->SetIssuer(srv.URL());
    oidc::ProviderConfig pc{.IssuerURL = string(srv.URL()),
                            .UserInfoURL = string(srv.URL() + "/userinfo"),
                            .JWKSURL = string(srv.URL() + "/keys")};
    auto [info, err3] = pc.NewProvider(bg())->UserInfo(bg(), oauth2::StaticTokenSource(make_ptr<oauth2::Token>()));
    EXPECT_TRUE(err3) << "a forged UserInfo response was accepted";
}

TEST(GoOIDC, ProviderClaimsAndNonce) {
    auto [priv, err] = ecdsa::GenerateKey(elliptic::P256(), nullptr);
    ASSERT_FALSE(err) << err;
    auto ts = newTestIssuer(priv);
    server srv(ts);
    ts->baseURL = srv.URL();
    auto [p, err2] = oidc::NewProvider(bg(), srv.URL());
    ASSERT_FALSE(err2) << err2;
    struct {
        std::string jwks_uri;
    } meta;
    ASSERT_FALSE(p->Claims(&meta));
    EXPECT_EQ(meta.jwks_uri, srv.URL() + "/keys");
    // A provider from a ProviderConfig has no discovery document.
    EXPECT_TRUE(oidc::ProviderConfig{}.NewProvider(bg())->Claims(&meta));

    auto opt = oidc::Nonce("n-0S6_WzA2Mj");
    ASSERT_EQ(opt.params.size(), 1u);
    EXPECT_EQ(S(opt.params[0].first), "nonce");
    EXPECT_EQ(S(opt.params[0].second), "n-0S6_WzA2Mj");
}
