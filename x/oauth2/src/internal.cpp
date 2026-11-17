// The token requests, the auth style cache and the context's HTTP client,
// ported from golang.org/x/oauth2 v0.36.0's internal/token.go and
// internal/oauth2.go. (internal/transport.go's ContextClient is here too;
// its ParseKey is not: it is for jws.)
#include "oauth2_internal.hpp"

#include <gopp/encoding/base64.hpp>
#include <gopp/io.hpp>
#include <gopp/mime.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <tuple>

namespace gopp::oauth2 {

const void* const HTTPClient = &HTTPClient;

ptr<context::Context> WithClient(ptr<context::Context> ctx, http::Client client) {
    auto cc = make_ptr<ContextClient>();
    cc->Client = std::move(client);
    return context::WithValue(std::move(ctx), HTTPClient, std::move(cc));
}

namespace internal {

std::function<time::Time()> timeNow = [] { return time::Now(); };

std::shared_ptr<AuthStyleCache> LazyAuthStyleCache::Get() const {
    if (auto c = v.load()) return c;
    auto c = std::make_shared<AuthStyleCache>();
    std::shared_ptr<AuthStyleCache> none;
    if (!v.compare_exchange_strong(none, c)) c = std::move(none);  // another fiber was first
    return c;
}

std::optional<oauth2::AuthStyle> AuthStyleCache::lookupAuthStyle(std::string_view tokenURL, std::string_view clientID) {
    std::lock_guard lock(mu_);
    auto it = m_.find({std::string(tokenURL), std::string(clientID)});
    if (it == m_.end()) return std::nullopt;
    return it->second;
}

void AuthStyleCache::setAuthStyle(std::string_view tokenURL, std::string_view clientID, oauth2::AuthStyle v) {
    std::lock_guard lock(mu_);
    m_[{std::string(tokenURL), std::string(clientID)}] = v;
}

http::Client ContextClient(const ptr<context::Context>& ctx) {
    if (ctx) {
        if (auto cc = dynamic_pointer_cast<oauth2::ContextClient>(ctx->Value(HTTPClient))) return cc->Client;
    }
    return http::DefaultClient();
}

namespace {

// The "expires_in" of a token response: seconds, in an int32. A server gives
// a number, or (PayPal's) a string of one; null is none.
struct expirationTime {
    int32_t seconds = 0;

    error UnmarshalJSON(std::span<const byte> b) {
        auto s = asStringView(b);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
        if (s.empty() || s == "null") return nullptr;
        std::string_view lit = s;
        if (s.front() == '"') {
            if (s.size() < 2 || s.back() != '"' || s.find('\\') != s.npos) return invalid(s);
            lit = s.substr(1, s.size() - 2);
        } else if (s.front() != '-' && (s.front() < '0' || s.front() > '9')) {
            return errors::New("json: cannot unmarshal " + std::string(s.substr(0, 20)) + " into Go value of type json.Number");
        }
        // json.Number.Int64: a JSON integer (no sign but '-', no leading zero).
        auto digits = lit.front() == '-' ? lit.substr(1) : lit;
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0') || digits.front() < '0' || digits.front() > '9')
            return invalid(s);
        auto [i, err] = strconv::ParseInt(lit, 10, 64);
        if (err) return err;
        // Go clamps above only, and wraps below; both are limited here.
        seconds = static_cast<int32_t>(std::clamp<int64>(i, INT32_MIN, INT32_MAX));
        return nullptr;
    }

private:
    static error invalid(std::string_view s) {
        return errors::New("json: invalid number literal, trying to unmarshal " + std::string(s.substr(0, 40)) + " into Number");
    }
};

// tokenJSON: a JSON token response, as RFC 6749, section 5.1 and 5.2 have it.
struct tokenJSON {
    [[= json::tag("access_token")]] string AccessToken;
    [[= json::tag("token_type")]] string TokenType;
    [[= json::tag("refresh_token")]] string RefreshToken;
    [[= json::tag("expires_in")]] expirationTime ExpiresIn;  // at least PayPal returns string, while most return number
    [[= json::tag("error")]] string ErrorCode;
    [[= json::tag("error_description")]] string ErrorDescription;
    [[= json::tag("error_uri")]] string ErrorURI;

    time::Time expiry() const {
        if (ExpiresIn.seconds != 0) return timeNow().Add(ExpiresIn.seconds * time::Second);
        return {};
    }
};

std::string basicAuth(std::string_view user, std::string_view password) {
    std::string cred = std::string(user) + ":" + std::string(password);
    return "Basic " + std::string(base64::StdEncoding.EncodeToString(as_bytes(cred)).view());
}

std::pair<ptr<http::Request>, error> newTokenRequest(std::string_view tokenURL, std::string_view clientID,
                                                     std::string_view clientSecret, url::Values v,
                                                     oauth2::AuthStyle authStyle) {
    if (authStyle == oauth2::AuthStyle::InParams) {
        if (!clientID.empty()) v.Set("client_id", string(clientID));
        if (!clientSecret.empty()) v.Set("client_secret", string(clientSecret));
    }
    auto [req, err] = http::NewRequest("POST", tokenURL, strings::NewReader(v.Encode()));
    if (err) return {nullptr, err};
    req->Header.Set("Content-Type", "application/x-www-form-urlencoded");
    if (authStyle == oauth2::AuthStyle::InHeader) {
        req->Header.Set("Authorization", string(basicAuth(url::QueryEscape(clientID).view(), url::QueryEscape(clientSecret).view())));
    }
    return {req, nullptr};
}

// What Atoi gives for an "expires_in" in seconds, which is 0 for no expiry,
// limited to an int32 where Go's would overflow a Duration.
int64 formExpiresIn(std::string_view e) {
    auto [n, err] = strconv::Atoi(e);  // a range error has the clamped value
    return std::clamp<int64>(n, INT32_MIN, INT32_MAX);
}

std::pair<ptr<Token>, error> doTokenRoundTrip(const ptr<context::Context>& ctx, ptr<http::Request> req) {
    auto client = ContextClient(ctx);
    auto [r, err] = client.Do(ctx ? req->WithContext(ctx) : req);
    if (err) return {nullptr, err};
    slice<byte> body;
    if (r->Body) {
        auto [b, rerr] = io::ReadAll(*io::LimitReader(r->Body, 1 << 20));
        r->Body->Close();
        if (rerr) return {nullptr, errors::New("oauth2: cannot fetch token: " + std::string(rerr.Error().view()))};
        body = std::move(b);
    }

    bool failureStatus = r->StatusCode < 200 || r->StatusCode > 299;
    auto retrieveError = make_ptr<RetrieveError>();
    retrieveError->Response = r;
    retrieveError->Body = body;

    auto tok = make_ptr<Token>();
    auto [content, params, merr] = mime::ParseMediaType(r->Header.Get("Content-Type").view());
    if (content.view() == "application/x-www-form-urlencoded" || content.view() == "text/plain") {
        auto [vals, perr] = url::ParseQuery(asStringView(body));
        if (perr) {
            if (failureStatus) return {nullptr, retrieveError};
            return {nullptr, errors::New("oauth2: cannot parse response: " + std::string(perr.Error().view()))};
        }
        retrieveError->ErrorCode = vals.Get("error");
        retrieveError->ErrorDescription = vals.Get("error_description");
        retrieveError->ErrorURI = vals.Get("error_uri");
        tok->AccessToken = vals.Get("access_token");
        tok->TokenType = vals.Get("token_type");
        tok->RefreshToken = vals.Get("refresh_token");
        if (int64 expires = formExpiresIn(vals.Get("expires_in").view()); expires != 0)
            tok->Expiry = timeNow().Add(expires * time::Second);
        tokenAccess::setForm(*tok, std::move(vals));
    } else {
        tokenJSON tj;
        if (error jerr = json::Unmarshal(body, &tj)) {
            if (failureStatus) return {nullptr, retrieveError};
            return {nullptr, errors::New("oauth2: cannot parse json: " + std::string(jerr.Error().view()))};
        }
        retrieveError->ErrorCode = tj.ErrorCode;
        retrieveError->ErrorDescription = tj.ErrorDescription;
        retrieveError->ErrorURI = tj.ErrorURI;
        tok->AccessToken = tj.AccessToken;
        tok->TokenType = tj.TokenType;
        tok->RefreshToken = tj.RefreshToken;
        tok->Expiry = tj.expiry();
        tok->ExpiresIn = tj.ExpiresIn.seconds;
        json::Value raw;
        json::Unmarshal(body, &raw);  // no error checks for optional fields
        tokenAccess::setRaw(*tok, std::move(raw));
    }
    if (failureStatus || !retrieveError->ErrorCode.empty()) return {nullptr, retrieveError};
    if (tok->AccessToken.empty()) return {nullptr, errors::New("oauth2: server response missing access_token")};
    return {tok, nullptr};
}

}  // namespace

std::pair<ptr<Token>, error> RetrieveToken(const ptr<context::Context>& ctx, std::string_view clientID,
                                           std::string_view clientSecret, std::string_view tokenURL,
                                           const url::Values& v, oauth2::AuthStyle authStyle,
                                           AuthStyleCache& styleCache) {
    bool needsAuthStyleProbe = authStyle == oauth2::AuthStyle::AutoDetect;
    if (needsAuthStyleProbe) {
        if (auto style = styleCache.lookupAuthStyle(tokenURL, clientID)) {
            authStyle = *style;
            needsAuthStyleProbe = false;
        } else {
            authStyle = oauth2::AuthStyle::InHeader;  // the first way we'll try
        }
    }
    auto [req, err] = newTokenRequest(tokenURL, clientID, clientSecret, v, authStyle);
    if (err) return {nullptr, err};
    auto [token, terr] = doTokenRoundTrip(ctx, req);
    if (terr && needsAuthStyleProbe) {
        // The second way we'll try.
        authStyle = oauth2::AuthStyle::InParams;
        std::tie(req, err) = newTokenRequest(tokenURL, clientID, clientSecret, v, authStyle);
        if (err) return {nullptr, err};
        std::tie(token, terr) = doTokenRoundTrip(ctx, req);
    }
    if (needsAuthStyleProbe && !terr) styleCache.setAuthStyle(tokenURL, clientID, authStyle);
    // Don't overwrite `RefreshToken` with an empty value if this was a token
    // refreshing request.
    if (token && token->RefreshToken.empty()) token->RefreshToken = v.Get("refresh_token");
    return {token, terr};
}

}  // namespace internal
}  // namespace gopp::oauth2
