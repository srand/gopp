// x/oidc, after github.com/coreos/go-oidc/v3 (v3.21.0): OpenID Connect
// discovery and ID token verification. The fake provider for tests is
// <gopp/oidc/oidctest.hpp>.
//
//     auto [provider, err] = oidc::NewProvider(ctx, "https://accounts.example.com");
//     oauth2::Config conf{.ClientID = id, .Endpoint = provider->Endpoint(),
//                         .Scopes = {string(oidc::ScopeOpenID)}};
//     auto verifier = provider->Verifier({.ClientID = id});
//
//     auto [tok, err2] = conf.Exchange(ctx, code);
//     json::Value raw = tok->Extra("id_token");
//     auto [idToken, err3] = verifier->Verify(ctx, raw.AsString().view());
//     struct { string email; bool email_verified = false; } claims;
//     err = idToken->Claims(&claims);
//
// Verify checks, in go-oidc's order: the token's "alg" against the allowed
// algorithms (Config::SupportedSigningAlgs, else the provider's, else
// RS256) as it parses it; the signature, with the key set; then the
// issuer, the audience (unless SkipClientIDCheck), the expiry (unless
// SkipExpiryCheck, with Config::Now) and "nbf" (with 5 minutes of leeway).
// It does not check the nonce: compare IDToken::Nonce yourself. Like
// go-oidc v3.21, it does not check "iat" either.
//
// The HTTP requests (discovery, the key set and UserInfo) go through the
// http::Client of the context, if it holds one (ClientContext), and else
// through http::DefaultClient(). ClientContext uses oauth2's key, so the
// context works for x/oauth2 too.
//
// Differences from go-oidc:
//  - http::Client isn't an object in go++, so ClientContext takes it by
//    value and stores it as an oauth2::ContextClient.
//  - The JSON of a token (the claims of an ID token, and what
//    IDToken::Claims decodes) is decoded with json::UnmarshalCaseSensitive,
//    as go-jose does: "ISS" is not "iss". go-oidc decodes it with
//    encoding/json, which matches keys case-insensitively. Discovery
//    documents and UserInfo are decoded as go-oidc does.
//  - The responses of the discovery, key set and UserInfo endpoints are
//    read up to 1 MiB (MaxResponseSize); a larger one fails. go-oidc reads
//    them whole.
//  - RemoteKeySet fetches the key set for an unknown key ID at most once
//    every 10 seconds (RemoteKeySetRefetchInterval): within that time of
//    the last fetch, it verifies with the keys it has. go-oidc fetches
//    again for every token with a key ID that it doesn't have, so a client
//    sending tokens with made-up key IDs makes it fetch for each.
//  - A verifier without a key set (a null ptr<KeySet>) fails, where Go's
//    panics; so does a StaticKeySet's null key.
//  - Config is copied into the verifier, where Go keeps a pointer to it.
//  - An "exp", "iat" or "nbf" of 2^62 seconds or more either way fails
//    the token, as go-jose's NumericDate does (Go's time.Unix wraps around
//    for such values, and its int64 conversion of a larger float is
//    undefined in C++).
//
// Left out: logout tokens (logout.go: VerifyLogout and LogoutToken) and
// distributed claims (the "_claim_names" and "_claim_sources" of a token
// are not read).
#pragma once

#include <gopp/base.hpp>
#include <gopp/context.hpp>
#include <gopp/crypto.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/jose.hpp>
#include <gopp/net/http.hpp>
#include <gopp/oauth2.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace gopp::oidc {

// The mandatory scope of OpenID Connect's OAuth 2.0 requests.
inline constexpr std::string_view ScopeOpenID = "openid";
// Asks for the user's profile claims, such as "name" and "picture".
inline constexpr std::string_view ScopeProfile = "profile";
// Asks for the "email" and "email_verified" claims.
inline constexpr std::string_view ScopeEmail = "email";
// Asks for a refresh token, where the provider supports it.
inline constexpr std::string_view ScopeOfflineAccess = "offline_access";

// The JOSE signature algorithms (RFC 7518) that go-oidc supports.
inline constexpr std::string_view RS256 = "RS256";  // RSASSA-PKCS-v1.5 using SHA-256
inline constexpr std::string_view RS384 = "RS384";  // RSASSA-PKCS-v1.5 using SHA-384
inline constexpr std::string_view RS512 = "RS512";  // RSASSA-PKCS-v1.5 using SHA-512
inline constexpr std::string_view ES256 = "ES256";  // ECDSA using P-256 and SHA-256
inline constexpr std::string_view ES384 = "ES384";  // ECDSA using P-384 and SHA-384
inline constexpr std::string_view ES512 = "ES512";  // ECDSA using P-521 and SHA-512
inline constexpr std::string_view PS256 = "PS256";  // RSASSA-PSS using SHA256 and MGF1-SHA256
inline constexpr std::string_view PS384 = "PS384";  // RSASSA-PSS using SHA384 and MGF1-SHA384
inline constexpr std::string_view PS512 = "PS512";  // RSASSA-PSS using SHA512 and MGF1-SHA512
inline constexpr std::string_view EdDSA = "EdDSA";  // Ed25519 using SHA-512

// The largest response of a discovery, key set or UserInfo endpoint that
// is read: 1 MiB (a go++ addition).
inline constexpr int64 MaxResponseSize = 1 << 20;
// How long after fetching its key set a RemoteKeySet doesn't fetch it again
// for an unknown key ID (a go++ addition).
inline constexpr time::Duration RemoteKeySetRefetchInterval = 10 * time::Second;

// oidc.ClientContext: ctx with client for the requests of this package
// (and of x/oauth2, whose key it uses).
ptr<context::Context> ClientContext(ptr<context::Context> ctx, http::Client client);

// oidc.InsecureIssuerURLContext: discovery accepts any issuer in the
// discovery document, and the provider then expects issuerURL in tokens.
// For off-spec providers such as Azure; it is insecure for multi-tenant
// providers, so review every use.
ptr<context::Context> InsecureIssuerURLContext(ptr<context::Context> ctx, std::string_view issuerURL);

// oidc.KeySet: verifies the signature of a JWT and returns its payload.
// It need not check the algorithm, issuer or audience: IDTokenVerifier
// does. A KeySet that makes HTTP requests should use the client of ctx.
struct KeySet : virtual object {
    virtual std::pair<slice<byte>, error> VerifySignature(ptr<context::Context> ctx, std::string_view jwt) = 0;
};

// oidc.StaticKeySet: verifies against a fixed set of keys, each a
// ptr<rsa::PublicKey>, ptr<ecdsa::PublicKey> or ptr<ed25519::PublicKey>.
struct StaticKeySet final : KeySet {
    slice<ptr<crypto::PublicKey>> PublicKeys;

    std::pair<slice<byte>, error> VerifySignature(ptr<context::Context> ctx, std::string_view jwt) override;
};

namespace internal {
struct remoteKeySetAccess;
}

// oidc.RemoteKeySet: verifies against the key set (JWKS) at a URL, which it
// caches. For a key ID that isn't cached, it fetches the set again (once
// for all the fibers that wait for it, as Go's inflight request), but not
// within RemoteKeySetRefetchInterval of the last fetch. Fetches send
// "Cache-Control: no-cache". Use one RemoteKeySet for all verifications.
class RemoteKeySet final : public KeySet {
public:
    // Don't call this directly: it doesn't check the algorithm. Use an
    // IDTokenVerifier.
    std::pair<slice<byte>, error> VerifySignature(ptr<context::Context> ctx, std::string_view jwt) override;

private:
    friend ptr<RemoteKeySet> NewRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL);
    friend struct internal::remoteKeySetAccess;
    struct inflight;

    // Only in a ptr: a fetch's fiber keeps the key set alive.
    RemoteKeySet() = default;
    std::pair<slice<byte>, error> verify(const ptr<context::Context>& ctx, const jose::JSONWebSignature& jws);
    slice<jose::JSONWebKey> keysFromCache();
    std::pair<slice<jose::JSONWebKey>, error> keysFromRemote(const ptr<context::Context>& ctx);
    std::pair<slice<jose::JSONWebKey>, error> updateKeys();

    string jwksURL_;
    std::optional<http::Client> client_;  // of the context given to NewRemoteKeySet
    std::function<time::Time()> now_;
    sync::RWMutex mu_;  // guards the fields below
    ptr<inflight> inflight_;
    slice<jose::JSONWebKey> cachedKeys_;
    bool fetched_ = false;  // whether a fetch has finished
    time::Time lastFetch_;  // when it finished
    error lastErr_;         // and its error
};

// oidc.NewRemoteKeySet: a key set fetched from jwksURL with the HTTP client
// of ctx. Only the client is taken from ctx: its cancellation is ignored.
ptr<RemoteKeySet> NewRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL);

// oidc.Config: the settings of an IDTokenVerifier.
struct Config {
    // The expected audience, usually the client ID that started the login.
    // Required unless SkipClientIDCheck.
    string ClientID{};
    // The algorithms allowed to sign a token. Empty: the provider's (see
    // Provider::Verifier), or else RS256.
    slice<string> SupportedSigningAlgs{};
    // Don't check the audience; must be set if ClientID is empty.
    bool SkipClientIDCheck = false;
    // Don't check the expiry (nor "nbf").
    bool SkipExpiryCheck = false;
    // Don't check the issuer. The caller MUST then check IDToken::Issuer.
    bool SkipIssuerCheck = false;
    // The clock of the expiry check; time::Now if null.
    std::function<time::Time()> Now{};
    // Don't verify the signature, and accept "none". Only for a token that
    // came straight from the provider's token endpoint, never for one from
    // anywhere else.
    bool InsecureSkipSignatureCheck = false;
};

namespace internal {
struct idTokenAccess;
struct providerAccess;
}  // namespace internal

// oidc.IDToken: a verified ID token. Use Claims for the claims it has no
// field for.
struct IDToken : object {
    // The issuer ("iss"). Google's tokens may say "accounts.google.com"
    // where the provider is "https://accounts.google.com".
    string Issuer;
    // The audience ("aud"), which contains the Config's ClientID.
    slice<string> Audience;
    // The user ("sub").
    string Subject;
    // "exp"; not after the verification time unless SkipExpiryCheck.
    time::Time Expiry;
    // "iat".
    time::Time IssuedAt;
    // "nonce": NOT checked by Verify; compare it with the one you sent.
    string Nonce;
    // "at_hash", for VerifyAccessToken.
    string AccessTokenHash;

    // i.Claims(v): decodes the token's payload into v, case-sensitively.
    template <typename T>
    error Claims(T* v) const {
        if (!hasClaims_) return errClaimsNotSet();
        return json::UnmarshalCaseSensitive(claims_, v);
    }
    error Claims(json::Value* v) const;
    // i.VerifyAccessToken(t): whether "at_hash" is the hash of t, with
    // the hash of the token's algorithm. An error if there's no at_hash.
    error VerifyAccessToken(std::string_view accessToken) const;

private:
    friend class IDTokenVerifier;
    friend struct internal::idTokenAccess;
    static error errClaimsNotSet();
    string sigAlgorithm_;
    slice<byte> claims_;
    bool hasClaims_ = false;
};

// oidc.IDTokenVerifier: verifies ID tokens for an issuer.
class IDTokenVerifier : public object {
public:
    // v.Verify(ctx, rawIDToken): parses the token and checks it, as said at
    // the top of this file. Expired is a TokenExpiredError.
    std::pair<ptr<IDToken>, error> Verify(ptr<context::Context> ctx, std::string_view rawIDToken) const;

private:
    friend ptr<IDTokenVerifier> NewVerifier(std::string_view issuerURL, ptr<KeySet> keySet, const Config& config);
    // The payload and the algorithm of a token whose signature verifies.
    std::tuple<slice<byte>, string, error> verifyJWT(ptr<context::Context> ctx, std::string_view rawIDToken) const;
    ptr<KeySet> keySet_;
    Config config_;
    string issuer_;
};

// oidc.NewVerifier: a verifier for tokens of issuerURL, signed by keys of
// keySet (a RemoteKeySet, or a StaticKeySet).
ptr<IDTokenVerifier> NewVerifier(std::string_view issuerURL, ptr<KeySet> keySet, const Config& config);

class Provider;
struct UserInfo;

// oidc.ProviderConfig: a provider's metadata, for providers without
// discovery; decodes from a discovery document.
struct ProviderConfig {
    // The issuer, exactly as in its tokens.
    [[= json::tag("issuer")]] string IssuerURL{};
    [[= json::tag("authorization_endpoint")]] string AuthURL{};
    [[= json::tag("token_endpoint")]] string TokenURL{};
    [[= json::tag("device_authorization_endpoint")]] string DeviceAuthURL{};
    [[= json::tag("userinfo_endpoint")]] string UserInfoURL{};
    [[= json::tag("jwks_uri")]] string JWKSURL{};
    // The algorithms allowed for ID tokens; empty: RS256 (see Config).
    [[= json::tag("id_token_signing_alg_values_supported")]] slice<string> Algorithms{};

    // p.NewProvider(ctx): a Provider of these endpoints, with the HTTP
    // client of ctx.
    ptr<Provider> NewProvider(ptr<context::Context> ctx) const;
};

// oidc.Provider: an OpenID Connect provider, from discovery or a
// ProviderConfig.
class Provider : public object {
public:
    // p.Endpoint(): the OAuth 2.0 endpoints, for an oauth2::Config.
    oauth2::Endpoint Endpoint() const;
    // p.UserInfoEndpoint(): the UserInfo URL; empty if there is none.
    string UserInfoEndpoint() const;
    // p.Verifier(config): a verifier with the provider's key set, shared by
    // the provider's verifiers, and its algorithms if the config has none.
    ptr<IDTokenVerifier> Verifier(const Config& config) const;
    // p.VerifierContext(ctx, config): Verifier, with a key set of its own
    // that uses the HTTP client of ctx.
    ptr<IDTokenVerifier> VerifierContext(ptr<context::Context> ctx, const Config& config) const;
    // p.Claims(v): decodes the discovery document into v; an error for a
    // provider from a ProviderConfig.
    template <typename T>
    error Claims(T* v) const {
        if (!hasRawClaims_) return errClaimsNotSet();
        return json::Unmarshal(rawClaims_, v);
    }
    // p.UserInfo(ctx, ts): the claims of the UserInfo endpoint, with a
    // token of ts. A signed (application/jwt) response is verified with the
    // provider's key set.
    std::pair<ptr<oidc::UserInfo>, error> UserInfo(ptr<context::Context> ctx, ptr<oauth2::TokenSource> ts) const;

private:
    friend struct ProviderConfig;
    friend std::pair<ptr<Provider>, error> NewProvider(ptr<context::Context> ctx, std::string_view issuer);
    friend struct internal::providerAccess;
    static error errClaimsNotSet();
    ptr<KeySet> remoteKeySet() const;
    ptr<IDTokenVerifier> newVerifier(ptr<KeySet> keySet, const Config& config) const;

    string issuer_, authURL_, tokenURL_, deviceAuthURL_, userInfoURL_, jwksURL_;
    slice<string> algorithms_;
    slice<byte> rawClaims_;
    bool hasRawClaims_ = false;
    std::optional<http::Client> client_;  // of the context given to NewProvider
    mutable sync::Mutex mu_;              // guards commonRemoteKeySet_
    mutable ptr<KeySet> commonRemoteKeySet_;
};

// oidc.NewProvider: discovers the provider of issuer from
// issuer + "/.well-known/openid-configuration". The document's issuer must
// be issuer exactly (IssuerMismatchError), unless InsecureIssuerURLContext.
// Algorithms that go-oidc doesn't support (HS256, none) are dropped.
std::pair<ptr<Provider>, error> NewProvider(ptr<context::Context> ctx, std::string_view issuer);

// oidc.UserInfo: the claims of the UserInfo endpoint.
struct UserInfo : object {
    string Subject;
    string Profile;
    string Email;
    // "email_verified", also when the provider sends it as a string.
    bool EmailVerified = false;

    // u.Claims(v): decodes the response into v.
    template <typename T>
    error Claims(T* v) const {
        if (!hasClaims_) return errClaimsNotSet();
        return json::Unmarshal(claims_, v);
    }

private:
    friend class Provider;
    static error errClaimsNotSet();
    slice<byte> claims_;
    bool hasClaims_ = false;
};

// oidc.Nonce: the authorization option that asks for a nonce in the ID
// token.
oauth2::AuthCodeOption Nonce(std::string_view nonce);

// oidc.TokenExpiredError: the token expired. It may be invalid in other
// ways too.
struct TokenExpiredError : errorInterface {
    time::Time Expiry;
    string Error() const override;
};

// oidc.IssuerMismatchError: the issuer of the discovery document isn't the
// one NewProvider was given (a trailing slash makes a difference).
struct IssuerMismatchError : errorInterface {
    // The issuer given to NewProvider.
    string Provided;
    // The issuer of the discovery document.
    string Discovered;
    string Error() const override;
};

}  // namespace gopp::oidc
