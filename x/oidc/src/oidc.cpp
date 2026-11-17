// Discovery, the provider, UserInfo and the ID token's methods, ported from
// coreos/go-oidc v3.21.0's oidc.go.
#include "oidc_internal.hpp"

#include <gopp/crypto/sha256.hpp>
#include <gopp/crypto/sha512.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/mime.hpp>
#include <gopp/strings.hpp>

#include <mutex>

namespace gopp::oidc {

namespace {

const error errNoAtHash = errors::New("id token did not have an access token hash");
const error errInvalidAtHash = errors::New("access token hash does not match value in ID token");
const error errClaimsNotSetValue = errors::New("oidc: claims not set");

// The context key of InsecureIssuerURLContext, and its value.
const void* const issuerURLKey = &issuerURLKey;
struct issuerURLValue final : object {
    string url;
};

// userInfoRaw's "email_verified": a bool, or a string that says one, as
// some providers (AWS Cognito) send it.
struct stringAsBool {
    bool v = false;
    error UnmarshalJSON(std::span<const byte> b) {
        std::string_view s = asStringView(b);
        if (s == "true" || s == R"("true")") {
            v = true;
        } else if (s == "false" || s == R"("false")") {
            v = false;
        } else {
            return errors::New("invalid value for boolean");
        }
        return nullptr;
    }
};

struct userInfoRaw {
    [[= json::tag("sub")]] string Subject;
    [[= json::tag("profile")]] string Profile;
    [[= json::tag("email")]] string Email;
    [[= json::tag("email_verified")]] stringAsBool EmailVerified;
};

}  // namespace

namespace internal {

bool supportedAlg(std::string_view alg) {
    for (auto a : allAlgs) {
        if (a == alg) return true;
    }
    return false;
}

std::optional<http::Client> getClient(const ptr<context::Context>& ctx) {
    if (ctx) {
        if (auto cc = dynamic_pointer_cast<oauth2::ContextClient>(ctx->Value(oauth2::HTTPClient))) return cc->Client;
    }
    return std::nullopt;
}

std::pair<ptr<http::Response>, error> doRequest(const std::optional<http::Client>& client,
                                                const ptr<context::Context>& ctx, ptr<http::Request> req) {
    http::Client c = client ? *client : http::DefaultClient();
    return c.Do(ctx ? req->WithContext(ctx) : req);
}

std::pair<ptr<http::Response>, error> doRequest(const ptr<context::Context>& ctx, ptr<http::Request> req) {
    return doRequest(getClient(ctx), ctx, std::move(req));
}

std::pair<slice<byte>, error> readBody(http::Response& r) {
    if (!r.Body) return {slice<byte>(), nullptr};
    auto [body, err] = io::ReadAll(*io::LimitReader(r.Body, MaxResponseSize + 1));
    r.Body->Close();
    if (err) return {slice<byte>(), err};
    if (static_cast<int64>(body.size()) > MaxResponseSize) {
        return {slice<byte>(), fmt::Errorf("oidc: response too large (more than {} bytes)", MaxResponseSize)};
    }
    return {body, nullptr};
}

error unmarshalResp(const http::Response& r, std::span<const byte> body, const std::function<error()>& unmarshal) {
    error err = unmarshal();
    if (!err) return nullptr;
    string ct = r.Header.Get("Content-Type");
    auto [mediaType, params, parseErr] = mime::ParseMediaType(ct.view());
    if (!parseErr && mediaType.view() == "application/json") {
        return fmt::Errorf("got Content-Type = application/json, but could not unmarshal as JSON: {}", err);
    }
    return fmt::Errorf("expected Content-Type = application/json, got {}: {}", strconv::Quote(ct.view()), err);
}

}  // namespace internal

ptr<context::Context> ClientContext(ptr<context::Context> ctx, http::Client client) {
    return oauth2::WithClient(std::move(ctx), std::move(client));
}

ptr<context::Context> InsecureIssuerURLContext(ptr<context::Context> ctx, std::string_view issuerURL) {
    auto v = make_ptr<issuerURLValue>();
    v->url = string(issuerURL);
    return context::WithValue(std::move(ctx), issuerURLKey, std::move(v));
}

// ---------------------------------------------------------------------------
// Provider

ptr<Provider> ProviderConfig::NewProvider(ptr<context::Context> ctx) const {
    auto p = make_ptr<Provider>();
    p->issuer_ = IssuerURL;
    p->authURL_ = AuthURL;
    p->tokenURL_ = TokenURL;
    p->deviceAuthURL_ = DeviceAuthURL;
    p->userInfoURL_ = UserInfoURL;
    p->jwksURL_ = JWKSURL;
    p->algorithms_ = Algorithms;
    p->client_ = internal::getClient(ctx);
    return p;
}

std::pair<ptr<Provider>, error> NewProvider(ptr<context::Context> ctx, std::string_view issuer) {
    string wellKnown = string(strings::TrimSuffix(issuer, "/")) + "/.well-known/openid-configuration";
    auto [req, err] = http::NewRequest("GET", wellKnown.view());
    if (err) return {nullptr, err};
    auto [resp, err2] = internal::doRequest(ctx, req);
    if (err2) return {nullptr, err2};
    auto [body, err3] = internal::readBody(*resp);
    if (err3) return {nullptr, fmt::Errorf("unable to read response body: {}", err3)};

    if (resp->StatusCode != http::StatusOK) return {nullptr, fmt::Errorf("{}: {}", resp->Status, asStringView(body))};

    ProviderConfig p;
    if (error e = internal::unmarshalResp(*resp, body, [&] { return json::Unmarshal(body, &p); })) {
        return {nullptr, fmt::Errorf("oidc: failed to decode provider discovery object: {}", e)};
    }

    string issuerURL(issuer);
    auto override = ctx ? dynamic_pointer_cast<issuerURLValue>(ctx->Value(issuerURLKey)) : nullptr;
    if (override) {
        issuerURL = override->url;
    } else if (p.IssuerURL != issuerURL) {
        auto e = make_ptr<IssuerMismatchError>();
        e->Provided = issuerURL;
        e->Discovered = p.IssuerURL;
        return {nullptr, e};
    }
    slice<string> algs;
    for (const auto& a : p.Algorithms) {
        if (internal::supportedAlg(a.view())) algs = append(algs, a);
    }
    auto provider = make_ptr<Provider>();
    provider->issuer_ = issuerURL;
    provider->authURL_ = p.AuthURL;
    provider->tokenURL_ = p.TokenURL;
    provider->deviceAuthURL_ = p.DeviceAuthURL;
    provider->userInfoURL_ = p.UserInfoURL;
    provider->jwksURL_ = p.JWKSURL;
    provider->algorithms_ = algs;
    provider->rawClaims_ = body;
    provider->hasRawClaims_ = true;
    provider->client_ = internal::getClient(ctx);
    return {provider, nullptr};
}

error Provider::errClaimsNotSet() { return errClaimsNotSetValue; }

oauth2::Endpoint Provider::Endpoint() const {
    return {.AuthURL = authURL_, .DeviceAuthURL = deviceAuthURL_, .TokenURL = tokenURL_};
}

string Provider::UserInfoEndpoint() const { return userInfoURL_; }

ptr<KeySet> Provider::remoteKeySet() const {
    std::lock_guard lock(mu_);
    if (!commonRemoteKeySet_) {
        auto ctx = context::Background();
        if (client_) ctx = ClientContext(ctx, *client_);
        commonRemoteKeySet_ = NewRemoteKeySet(ctx, jwksURL_.view());
    }
    return commonRemoteKeySet_;
}

std::pair<ptr<UserInfo>, error> Provider::UserInfo(ptr<context::Context> ctx, ptr<oauth2::TokenSource> ts) const {
    if (userInfoURL_.empty()) return {nullptr, errors::New("oidc: user info endpoint is not supported by this provider")};

    auto [req, err] = http::NewRequest("GET", userInfoURL_.view());
    if (err) return {nullptr, fmt::Errorf("oidc: create GET request: {}", err)};

    if (!ts) return {nullptr, errors::New("oidc: get access token: no token source")};
    auto [token, err2] = ts->Token();
    if (err2) return {nullptr, fmt::Errorf("oidc: get access token: {}", err2)};
    if (!token) return {nullptr, errors::New("oidc: get access token: no token")};
    token->SetAuthHeader(*req);

    auto [resp, err3] = internal::doRequest(ctx, req);
    if (err3) return {nullptr, err3};
    auto [body, err4] = internal::readBody(*resp);
    if (err4) return {nullptr, err4};
    if (resp->StatusCode != http::StatusOK) return {nullptr, fmt::Errorf("{}: {}", resp->Status, asStringView(body))};

    auto [mediaType, params, parseErr] = mime::ParseMediaType(resp->Header.Get("Content-Type").view());
    if (!parseErr && mediaType.view() == "application/jwt") {
        auto [payload, err5] = remoteKeySet()->VerifySignature(ctx, asStringView(body));
        if (err5) return {nullptr, fmt::Errorf("oidc: invalid userinfo jwt signature {}", err5)};
        body = payload;
    }

    userInfoRaw userInfo;
    if (error e = json::Unmarshal(body, &userInfo)) return {nullptr, fmt::Errorf("oidc: failed to decode userinfo: {}", e)};
    auto u = make_ptr<oidc::UserInfo>();
    u->Subject = userInfo.Subject;
    u->Profile = userInfo.Profile;
    u->Email = userInfo.Email;
    u->EmailVerified = userInfo.EmailVerified.v;
    u->claims_ = body;
    u->hasClaims_ = true;
    return {u, nullptr};
}

error UserInfo::errClaimsNotSet() { return errClaimsNotSetValue; }

// ---------------------------------------------------------------------------
// IDToken

error IDToken::errClaimsNotSet() { return errClaimsNotSetValue; }

error IDToken::Claims(json::Value* v) const {
    if (!hasClaims_) return errClaimsNotSet();
    return json::UnmarshalCaseSensitive(claims_, v);
}

error IDToken::VerifyAccessToken(std::string_view accessToken) const {
    if (AccessTokenHash.empty()) return errNoAtHash;
    crypto::Hash h;
    std::string_view alg = sigAlgorithm_.view();
    if (alg == RS256 || alg == ES256 || alg == PS256) {
        h = crypto::Hash::SHA256;
    } else if (alg == RS384 || alg == ES384 || alg == PS384) {
        h = crypto::Hash::SHA384;
    } else if (alg == RS512 || alg == ES512 || alg == PS512 || alg == EdDSA) {
        h = crypto::Hash::SHA512;
    } else {
        return fmt::Errorf("oidc: unsupported signing algorithm {}", strconv::Quote(alg));
    }
    auto hh = crypto::New(h);
    hh->Write(as_bytes(accessToken));
    slice<byte> sum = hh->Sum(slice<byte>());
    std::span<const byte> half(sum.data(), sum.size() / 2);
    if (base64::RawURLEncoding.EncodeToString(half) != AccessTokenHash) return errInvalidAtHash;
    return nullptr;
}

oauth2::AuthCodeOption Nonce(std::string_view nonce) { return oauth2::SetAuthURLParam("nonce", nonce); }

string TokenExpiredError::Error() const { return fmt::Sprint("oidc: token is expired (Token Expiry: {})", Expiry); }

string IssuerMismatchError::Error() const {
    return fmt::Sprint(
        "oidc: issuer URL provided to client ({}) did not match the issuer URL returned by provider ({})",
        strconv::Quote(Provided.view()), strconv::Quote(Discovered.view()));
}

}  // namespace gopp::oidc
