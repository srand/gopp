// Tokens, ported from golang.org/x/oauth2 v0.36.0's token.go.
#include "oauth2_internal.hpp"

#include <gopp/strings.hpp>

namespace gopp::oauth2 {

namespace {

// defaultExpiryDelta: how long before its Expiry a token is no longer valid,
// so that it doesn't expire on its way to the server.
constexpr time::Duration defaultExpiryDelta = 10 * time::Second;

}  // namespace

string Token::Type() const {
    if (strings::EqualFold(TokenType.view(), "bearer")) return "Bearer";
    if (strings::EqualFold(TokenType.view(), "mac")) return "MAC";
    if (strings::EqualFold(TokenType.view(), "basic")) return "Basic";
    if (!TokenType.empty()) return TokenType;
    return "Bearer";
}

void Token::SetAuthHeader(http::Request& r) const {
    r.Header.Set("Authorization", Type() + " " + AccessToken);
}

ptr<Token> Token::WithExtra(json::Value raw) const {
    auto t2 = make_ptr<Token>(*this);
    t2->raw_ = std::move(raw);
    t2->form_.reset();
    return t2;
}

json::Value Token::Extra(std::string_view key) const {
    if (raw_.IsObject()) return raw_[key];
    if (!form_) return nullptr;

    // A form-encoded value is a number if it looks like one.
    string v = form_->Get(key);
    auto s = strings::TrimSpace(v);
    switch (strings::Count(s.view(), ".")) {
    case 0:  // Contains no "."; try to parse as int
        if (auto [i, err] = strconv::ParseInt(s.view(), 10, 64); !err) return json::Value(static_cast<double>(i));
        break;
    case 1:  // Contains a single "."; try to parse as float
        if (auto [f, err] = strconv::ParseFloat(s.view(), 64); !err) return json::Value(f);
        break;
    }
    return json::Value(std::move(v));
}

namespace internal {

bool tokenAccess::expired(const Token& t) {
    if (t.Expiry.IsZero()) return false;
    time::Duration expiryDelta = defaultExpiryDelta;
    if (t.expiryDelta_.ns != 0) expiryDelta = t.expiryDelta_;
    return t.Expiry.Add(-expiryDelta).Before(timeNow());
}

std::pair<ptr<Token>, error> retrieveToken(const ptr<context::Context>& ctx, const Config& c, const url::Values& v) {
    return RetrieveToken(ctx, c.ClientID.view(), c.ClientSecret.view(), c.Endpoint.TokenURL.view(), v,
                         c.Endpoint.AuthStyle, *c.authStyleCache.Get());
}

}  // namespace internal

bool Token::Valid() const { return !AccessToken.empty() && !internal::tokenAccess::expired(*this); }

string RetrieveError::Error() const {
    if (!ErrorCode.empty()) {
        std::string s = "oauth2: " + std::string(strconv::Quote(ErrorCode.view()).view());
        if (!ErrorDescription.empty()) s += " " + std::string(strconv::Quote(ErrorDescription.view()).view());
        if (!ErrorURI.empty()) s += " " + std::string(strconv::Quote(ErrorURI.view()).view());
        return s;
    }
    return "oauth2: cannot fetch token: " + (Response ? Response->Status : string()) + "\nResponse: " +
           string(asStringView(Body));
}

}  // namespace gopp::oauth2
