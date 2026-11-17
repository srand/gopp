// x/oauth2, after golang.org/x/oauth2 (v0.36.0): an OAuth 2.0 client.
//
//     oauth2::Config conf{.ClientID = "id", .ClientSecret = "secret",
//                         .Endpoint = {.AuthURL = ".../auth", .TokenURL = ".../token"},
//                         .RedirectURL = "https://app/callback", .Scopes = {"openid"}};
//
//     // Authorization code flow, with PKCE (RFC 7636):
//     string verifier = oauth2::GenerateVerifier();
//     auto url = conf.AuthCodeURL(state, std::array{oauth2::S256ChallengeOption(verifier)});
//     auto [tok, err] = conf.Exchange(ctx, code, std::array{oauth2::VerifierOption(verifier)});
//
//     // A client that sets the token and refreshes it when it expires:
//     auto client = conf.Client(ctx, tok);
//
//     // Device flow (RFC 8628):
//     auto [da, err2] = conf.DeviceAuth(ctx);
//     auto [tok2, err3] = conf.DeviceAccessToken(ctx, *da);
//
// The requests go through the http::Client in ctx, if it holds one under the
// key HTTPClient (a ContextClient), and else through http::DefaultClient().
//
// Differences from Go's:
//  - http::Client isn't an object here, so Config::Client and NewClient
//    return it by value (a Client is a transport, a timeout and a redirect
//    limit), and a ContextClient holds one. Go's CheckRedirect and Jar don't
//    exist in go++'s Client.
//  - A Token's extra values (Token::Extra) are a json::Value, so that a
//    number is always a double, where Go's Extra of a form-encoded response
//    returns an int64 for an integer.
//  - The "expires_in" of a token response is limited to the range of an
//    int32 (Go's JSON path limits it above only, and wraps a form-encoded
//    one that doesn't fit an int).
//  - Config::TokenSource keeps a copy of the Config, not a pointer to it.
//  - A null Token is not Valid() (Go's nil *Token), but it is a call on a
//    null ptr in C++: check the ptr first.
//  - DeviceAuth honors ctx for its request and closes the response body.
//
// The fields of Config and Endpoint have initializers so that a designated
// initializer can leave fields out under -Wmissing-field-initializers.
//
// Left out: the endpoints and provider subpackages, jws, jwt, google,
// clientcredentials, authhandler, and the deprecated NoContext,
// RegisterBrokenAuthHeaderProvider and Transport::CancelRequest.
#pragma once

#include <gopp/base.hpp>
#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/url.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace gopp::oauth2 {

// oauth2.AuthStyle: how the client ID and secret are sent to the token
// endpoint.
enum class AuthStyle {
    // AutoDetect tries the header first, then the parameters, and remembers
    // what worked for the token URL and client ID.
    AutoDetect = 0,
    // InParams sends them as client_id and client_secret in the body.
    InParams = 1,
    // InHeader sends them as HTTP Basic authentication.
    InHeader = 2,
};

// oauth2.Endpoint: the URLs of an authorization server.
struct Endpoint {
    string AuthURL{};
    string DeviceAuthURL{};
    string TokenURL{};
    // How the token endpoint wants the client's credentials.
    oauth2::AuthStyle AuthStyle = oauth2::AuthStyle::AutoDetect;
};

struct Token;
namespace internal {
struct tokenAccess;
}

// oauth2.Token: what an authorization server hands out.
struct Token : object {
    // Sent as the bearer token (or of another type, see TokenType).
    string AccessToken;
    // "Bearer" if the server doesn't say, see Type().
    string TokenType;
    // Used to get a new AccessToken once this one expires.
    string RefreshToken;
    // When the AccessToken expires; the zero time if it doesn't.
    time::Time Expiry;
    // The "expires_in" of the response: seconds, as the server gave them.
    int64 ExpiresIn = 0;

    // t.Valid(): whether the AccessToken is set and (when the token has an
    // expiry) not about to expire: within 10 seconds, or the delta of
    // ReuseTokenSourceWithExpiry.
    bool Valid() const;
    // t.Type(): TokenType, with "bearer", "mac" and "basic" in their usual
    // case, and "Bearer" if empty.
    string Type() const;
    // t.SetAuthHeader(r): sets the Authorization header of r.
    void SetAuthHeader(http::Request& r) const;
    // t.Extra(key): a field of the response that the Token has no field
    // for, such as "id_token"; null if there is none.
    json::Value Extra(std::string_view key) const;
    // t.WithExtra(raw): a copy of t with raw as its extra values (an
    // object).
    ptr<Token> WithExtra(json::Value raw) const;

private:
    friend struct internal::tokenAccess;
    json::Value raw_;                     // the response of a JSON token endpoint
    std::optional<url::Values> form_;     // the response of a form-encoded one
    time::Duration expiryDelta_{};        // 0: the default, 10 seconds
};

// oauth2.TokenSource: a source of tokens, as the Transport and the Client
// need them. Implementations should be safe for use from several fibers.
struct TokenSource : virtual object {
    virtual std::pair<ptr<oauth2::Token>, error> Token() = 0;
};

// oauth2.AuthCodeOption: a parameter of an authorization URL or of a token
// request.
struct AuthCodeOption {
    std::vector<std::pair<string, string>> params;
};

// oauth2.SetAuthURLParam: the option that sets one parameter.
AuthCodeOption SetAuthURLParam(std::string_view key, std::string_view value);

// oauth2.AccessTypeOnline, AccessTypeOffline and ApprovalForce.
extern const AuthCodeOption AccessTypeOnline, AccessTypeOffline, ApprovalForce;

// oauth2.GenerateVerifier: a random PKCE code verifier (RFC 7636): 32 bytes,
// as 43 characters of unpadded base64url.
string GenerateVerifier();
// oauth2.VerifierOption: Exchange's code_verifier parameter.
AuthCodeOption VerifierOption(std::string_view verifier);
// oauth2.S256ChallengeFromVerifier: BASE64URL(SHA256(verifier)).
string S256ChallengeFromVerifier(std::string_view verifier);
// oauth2.S256ChallengeOption: AuthCodeURL's code_challenge and
// code_challenge_method=S256 parameters for the verifier.
AuthCodeOption S256ChallengeOption(std::string_view verifier);

// oauth2.DeviceAuthResponse: the response of a device authorization
// endpoint (RFC 8628, section 3.2), as JSON with these names:
// device_code, user_code, verification_uri (or verification_url, as Google
// sends it), verification_uri_complete, expires_in and interval.
struct DeviceAuthResponse : object {
    // The device verification code.
    string DeviceCode;
    // The end-user verification code.
    string UserCode;
    // The end-user verification URI on the authorization server.
    string VerificationURI;
    // A verification URI that includes the user code, if the server has one.
    string VerificationURIComplete;
    // When the device code expires (the "expires_in" of the JSON); the zero
    // time if the response has none.
    time::Time Expiry;
    // The seconds to wait between polls; 0 means the default, 5.
    int64 Interval = 0;

    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> data);
};

namespace internal {

struct AuthStyleCache;

// internal.LazyAuthStyleCache: a Config's cache of the auth style of each
// token URL and client ID, created when first used. A copy of a Config
// shares the cache of the original, if that has one by then.
struct LazyAuthStyleCache {
    LazyAuthStyleCache() = default;
    LazyAuthStyleCache(const LazyAuthStyleCache& o) : v(o.v.load()) {}
    LazyAuthStyleCache& operator=(const LazyAuthStyleCache& o) {
        v.store(o.v.load());
        return *this;
    }
    std::shared_ptr<AuthStyleCache> Get() const;

    mutable std::atomic<std::shared_ptr<AuthStyleCache>> v;
};

}  // namespace internal

// oauth2.Config: the settings of an OAuth 2.0 client, with the authorization
// server's endpoint. Use a copy of one Config in each fiber, or don't change
// it once it is in use.
struct Config {
    // The application's ID.
    string ClientID{};
    // The application's secret; may be empty.
    string ClientSecret{};
    // The server's URLs.
    oauth2::Endpoint Endpoint{};
    // Where the server redirects to after authorization.
    string RedirectURL{};
    // The scopes to request; may be empty.
    slice<string> Scopes{};

    // The auth styles found for this Config's token endpoint; not for use.
    internal::LazyAuthStyleCache authStyleCache{};

    // conf.AuthCodeURL(state, opts): the URL of the consent page. The state
    // protects against CSRF: check it when the user comes back.
    string AuthCodeURL(std::string_view state, std::span<const AuthCodeOption> opts = {}) const;
    // conf.Exchange(ctx, code, opts): converts the authorization code into a
    // token (with VerifierOption if the authorization used PKCE).
    std::pair<ptr<Token>, error> Exchange(ptr<context::Context> ctx, std::string_view code,
                                          std::span<const AuthCodeOption> opts = {}) const;
    // conf.PasswordCredentialsToken: a token for the user's password
    // (RFC 6749, section 4.3), which the RFC discourages.
    std::pair<ptr<Token>, error> PasswordCredentialsToken(ptr<context::Context> ctx, std::string_view username,
                                                          std::string_view password) const;
    // conf.TokenSource(ctx, t): a source that returns t while it is valid
    // and then refreshes it with its RefreshToken. A refresh that returns no
    // new refresh token keeps the old one. A null t is expired.
    ptr<oauth2::TokenSource> TokenSource(ptr<context::Context> ctx, ptr<Token> t) const;
    // conf.Client(ctx, t): NewClient(ctx, conf.TokenSource(ctx, t)).
    http::Client Client(ptr<context::Context> ctx, ptr<Token> t) const;
    // conf.DeviceAuth(ctx, opts): starts the device flow (RFC 8628).
    std::pair<ptr<DeviceAuthResponse>, error> DeviceAuth(ptr<context::Context> ctx,
                                                         std::span<const AuthCodeOption> opts = {}) const;
    // conf.DeviceAccessToken(ctx, da, opts): polls the token endpoint every
    // da.Interval seconds (5 if 0) until the user has authorized, adding 5
    // seconds on "slow_down", and until da.Expiry or ctx is done.
    std::pair<ptr<Token>, error> DeviceAccessToken(ptr<context::Context> ctx, const DeviceAuthResponse& da,
                                                   std::span<const AuthCodeOption> opts = {}) const;
};

// oauth2.StaticTokenSource: always returns t, never refreshes it.
ptr<TokenSource> StaticTokenSource(ptr<Token> t);
// oauth2.ReuseTokenSource: returns t while it is valid, and else a token
// from src, which it keeps for the next call. If src is already a
// ReuseTokenSource and t is null, it is returned as it is.
ptr<TokenSource> ReuseTokenSource(ptr<Token> t, ptr<TokenSource> src);
// oauth2.ReuseTokenSourceWithExpiry: as ReuseTokenSource, with tokens
// treated as expired earlyExpiry before their Expiry.
ptr<TokenSource> ReuseTokenSourceWithExpiry(ptr<Token> t, ptr<TokenSource> src, time::Duration earlyExpiry);

// oauth2.HTTPClient: the context key of the http::Client that requests to
// the token endpoints use (see WithClient), as a ContextClient.
extern const void* const HTTPClient;

// The value of the HTTPClient key. (go++'s context values are objects.)
struct ContextClient : object {
    http::Client Client;
};

// context.WithValue(ctx, oauth2.HTTPClient, client)
ptr<context::Context> WithClient(ptr<context::Context> ctx, http::Client client);

// oauth2.NewClient: an http::Client whose requests carry the token of src,
// through a Transport on the transport of the HTTPClient of ctx (or
// http::DefaultClient()'s), with its timeout and redirect limit. Null src:
// the HTTPClient of ctx.
http::Client NewClient(ptr<context::Context> ctx, ptr<TokenSource> src);

// oauth2.Transport: a RoundTripper that sets the Authorization header of
// each request from a TokenSource.
struct Transport final : http::RoundTripper {
    // The token source; the Transport fails without one.
    ptr<oauth2::TokenSource> Source;
    // Makes the requests; http::DefaultTransport() if null.
    ptr<http::RoundTripper> Base;

    std::pair<ptr<http::Response>, error> RoundTrip(http::Request& req) override;
};

// oauth2.RetrieveError: a token or device authorization endpoint's error
// response (a non-2xx status, or an "error" in the body).
struct RetrieveError : errorInterface {
    // The response, whose Body is spent: see Body.
    ptr<http::Response> Response;
    // The body of the response, up to 1 MiB.
    slice<byte> Body;
    // The error, error_description and error_uri of the body, as RFC 6749,
    // section 5.2, has them.
    string ErrorCode, ErrorDescription, ErrorURI;

    string Error() const override;
};

}  // namespace gopp::oauth2
