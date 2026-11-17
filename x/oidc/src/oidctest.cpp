// The fake provider, ported from coreos/go-oidc v3.21.0's
// oidctest/oidctest.go.
#include <gopp/oidc/oidctest.hpp>

#include <gopp/encoding/json.hpp>
#include <gopp/fmt.hpp>
#include <gopp/jose.hpp>
#include <gopp/path.hpp>

#include <string>
#include <utility>
#include <vector>

namespace gopp::oidctest {

namespace {

std::pair<string, error> newToken(ptr<crypto::PrivateKey> priv, std::string_view keyID, std::string_view alg,
                                  std::string_view claims) {
    jose::SigningKey key{alg, std::move(priv)};
    jose::SignerOptions opts;
    opts.ExtraHeaders["kid"] = json::Value(keyID);
    auto [signer, err] = jose::NewSigner(key, &opts);
    if (err) return {"", fmt::Errorf("creating signer: {}", err)};
    auto [sig, err2] = signer->Sign(as_bytes(claims));
    if (err2) return {"", fmt::Errorf("signing payload: {}", err2)};
    auto [jwt, err3] = sig->CompactSerialize();
    if (err3) return {"", fmt::Errorf("serializing jwt: {}", err3)};
    return {jwt, nullptr};
}

// u.JoinPath(elem): u with elem joined to its path.
string joinPath(url::URL u, std::string_view elem) {
    std::string base = "/" + std::string(u.Path.view());
    u.Path = path::Join({std::string_view(base), elem});
    u.RawPath = "";
    return u.String();
}

struct discovery {
    [[= json::tag("issuer")]] string Issuer;
    [[= json::tag("authorization_endpoint")]] string Auth;
    [[= json::tag("token_endpoint")]] string Token;
    [[= json::tag("jwks_uri")]] string JWKs;
    [[= json::tag("response_types_supported")]] std::vector<std::string> ResponseTypes;
    [[= json::tag("subject_types_supported")]] std::vector<std::string> SubjectTypes;
    [[= json::tag("id_token_signing_alg_values_supported")]] slice<string> Algs;
};

}  // namespace

string SignIDToken(ptr<crypto::PrivateKey> priv, std::string_view keyID, std::string_view alg, std::string_view claims) {
    auto [token, err] = newToken(std::move(priv), keyID, alg, claims);
    if (err) panic("oidctest: generating token: " + std::string(err.Error().view()));
    return token;
}

void Server::SetIssuer(std::string_view issuerURL) {
    auto [u, err] = url::Parse(issuerURL);
    if (err) panic("oidctest: invalid issuer URL: " + std::string(err.Error().view()));
    issuerURL_ = std::move(u);
}

void Server::ServeHTTP(http::ResponseWriter& w, http::Request& r) {
    std::string_view p = r.URL.Path.view();
    if (p == "/.well-known/openid-configuration") {
        if (r.Method.view() != http::MethodGet) {
            http::Error(w, "Expected GET request for discovery endpoint, got: " + std::string(r.Method.view()),
                        http::StatusMethodNotAllowed);
            return;
        }
        serveDiscovery(w, r);
    } else if (p == "/keys") {
        if (r.Method.view() != http::MethodGet) {
            http::Error(w, "Expected GET request for keys endpoint, got: " + std::string(r.Method.view()),
                        http::StatusMethodNotAllowed);
            return;
        }
        serveKeys(w, r);
    } else {
        http::Error(w, "Unknown path: " + std::string(p), http::StatusNotFound);
    }
}

void Server::serveDiscovery(http::ResponseWriter& w, http::Request&) {
    if (!issuerURL_) {
        http::Error(w, "oidctest: server called without SetIssuer()", http::StatusInternalServerError);
        return;
    }
    slice<string> algs = Algorithms;
    if (algs.empty()) algs = {"RS256"};
    discovery disc{
        .Issuer = issuerURL_->String(),
        .Auth = joinPath(*issuerURL_, "/auth"),
        .Token = joinPath(*issuerURL_, "/token"),
        .JWKs = joinPath(*issuerURL_, "/keys"),
        .ResponseTypes = {"code", "id_token", "token id_token"},
        .SubjectTypes = {"public"},
        .Algs = algs,
    };
    w.Header().Set("Content-Type", "application/json");
    json::Encode(w, disc);
}

void Server::serveKeys(http::ResponseWriter& w, http::Request&) {
    jose::JSONWebKeySet set;
    for (const auto& pub : PublicKeys) {
        jose::JSONWebKey k;
        k.Key = pub.PublicKey;
        k.KeyID = pub.KeyID;
        k.Algorithm = pub.Algorithm;
        k.Use = "sig";
        set.Keys = append(set.Keys, k);
    }
    w.Header().Set("Content-Type", "application/json");
    json::Encode(w, set);
}

}  // namespace gopp::oidctest
