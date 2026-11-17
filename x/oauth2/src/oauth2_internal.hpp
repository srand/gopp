// x/oauth2's internals: golang.org/x/oauth2/internal (the token requests, the
// auth style cache and the context's HTTP client) and the private parts of
// the oauth2 package that its files share.
#pragma once

#include <gopp/oauth2.hpp>
#include <gopp/sync.hpp>

#include <functional>
#include <map>
#include <string>

namespace gopp::oauth2::internal {

// oauth2.timeNow: the clock of Token::Valid; tests replace it.
extern std::function<time::Time()> timeNow;

// What the oauth2 package does with a Token's private fields.
struct tokenAccess {
    static bool expired(const Token& t);
    static void setExpiryDelta(Token& t, time::Duration d) { t.expiryDelta_ = d; }
    static void setRaw(Token& t, json::Value raw) { t.raw_ = std::move(raw); }
    static void setForm(Token& t, url::Values form) { t.form_ = std::move(form); }
};

// internal.AuthStyleCache: the auth style found for each token URL and
// client ID.
struct AuthStyleCache {
    std::optional<oauth2::AuthStyle> lookupAuthStyle(std::string_view tokenURL, std::string_view clientID);
    void setAuthStyle(std::string_view tokenURL, std::string_view clientID, oauth2::AuthStyle v);

private:
    sync::Mutex mu_;
    std::map<std::pair<std::string, std::string>, oauth2::AuthStyle> m_;
};

// internal.ContextClient: the http::Client of the HTTPClient key of ctx, or
// a copy of http::DefaultClient().
http::Client ContextClient(const ptr<context::Context>& ctx);

// internal.RetrieveToken: POSTs v to tokenURL with the client's credentials
// in the way authStyle says (probing, with the cache, if AutoDetect), and
// parses the response. The body is read up to 1 MiB.
std::pair<ptr<Token>, error> RetrieveToken(const ptr<context::Context>& ctx, std::string_view clientID,
                                           std::string_view clientSecret, std::string_view tokenURL,
                                           const url::Values& v, oauth2::AuthStyle authStyle,
                                           AuthStyleCache& styleCache);

// retrieveToken: RetrieveToken with the Config's settings.
std::pair<ptr<Token>, error> retrieveToken(const ptr<context::Context>& ctx, const Config& c, const url::Values& v);

// The error codes of the device flow.
inline constexpr std::string_view errAuthorizationPending = "authorization_pending";
inline constexpr std::string_view errSlowDown = "slow_down";
inline constexpr std::string_view errAccessDenied = "access_denied";
inline constexpr std::string_view errExpiredToken = "expired_token";

}  // namespace gopp::oauth2::internal
