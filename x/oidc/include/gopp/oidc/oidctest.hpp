// x/oidc's oidctest, after github.com/coreos/go-oidc/v3/oidc/oidctest
// (v3.21.0): a fake OpenID Connect provider for tests. It panics where it
// can't go on; not for use outside tests.
//
//     auto [priv, err] = rsa::GenerateKey(nullptr, 2048);
//     auto s = make_ptr<oidctest::Server>();
//     s->PublicKeys = {{.PublicKey = priv->Public(), .KeyID = "my-key-id", .Algorithm = string(oidc::RS256)}};
//     auto srv = httptest::NewServer(s);
//     s->SetIssuer(srv->URL.view());
//
//     string token = oidctest::SignIDToken(priv, "my-key-id", oidc::RS256,
//         R"({"iss": ")" + ... + R"(", "aud": "my-client-id", "sub": "foo", "exp": ...})");
//     auto [p, err2] = oidc::NewProvider(ctx, srv->URL.view());
//     auto [idToken, err3] = p->VerifierContext(ctx, {.ClientID = "my-client-id"})->Verify(ctx, token.view());
//
// The server serves the discovery document (/.well-known/openid-configuration,
// with /auth, /token and /keys under the issuer) and the key set (/keys).
// Difference from Go's: SignIDToken takes the key as a ptr, as go++'s keys
// are reference-counted objects.
#pragma once

#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/url.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <optional>
#include <string_view>

namespace gopp::oidctest {

// oidctest.SignIDToken: claims (JSON) signed with priv as a compact JWS
// whose header has alg and the "kid" keyID. Panics if it can't sign.
string SignIDToken(ptr<crypto::PrivateKey> priv, std::string_view keyID, std::string_view alg,
                   std::string_view claims);

// oidctest.PublicKey: a key of the server's key set.
struct PublicKey {
    // A ptr<rsa::PublicKey>, ptr<ecdsa::PublicKey> or ptr<ed25519::PublicKey>.
    ptr<crypto::PublicKey> PublicKey{};
    // The key ID, as given to SignIDToken.
    string KeyID{};
    // The key's algorithm, such as "RS256".
    string Algorithm{};
};

// oidctest.Server: the fake provider, an http::Handler. Set its fields and
// call SetIssuer before it serves.
class Server : public http::Handler {
public:
    // The keys of the key set.
    slice<oidctest::PublicKey> PublicKeys;
    // The signing algorithms of the discovery document; "RS256" if empty.
    slice<string> Algorithms;

    // s.SetIssuer(issuerURL): the issuer, usually the httptest server's
    // URL. Panics if it isn't a URL.
    void SetIssuer(std::string_view issuerURL);
    void ServeHTTP(http::ResponseWriter& w, http::Request& r) override;

private:
    void serveDiscovery(http::ResponseWriter& w, http::Request& r);
    void serveKeys(http::ResponseWriter& w, http::Request& r);
    std::optional<url::URL> issuerURL_;
};

}  // namespace gopp::oidctest
