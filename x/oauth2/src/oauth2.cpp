// The OAuth 2.0 client, ported from golang.org/x/oauth2 v0.36.0's oauth2.go.
#include "oauth2_internal.hpp"

#include <gopp/strings.hpp>

#include <mutex>

namespace gopp::oauth2 {

AuthCodeOption SetAuthURLParam(std::string_view key, std::string_view value) {
    return {{{string(key), string(value)}}};
}

const AuthCodeOption AccessTypeOnline = SetAuthURLParam("access_type", "online");
const AuthCodeOption AccessTypeOffline = SetAuthURLParam("access_type", "offline");
const AuthCodeOption ApprovalForce = SetAuthURLParam("prompt", "consent");

namespace {

void setValues(url::Values& v, std::span<const AuthCodeOption> opts) {
    for (auto& opt : opts)
        for (auto& [k, val] : opt.params) v.Set(k, val);
}

string joinScopes(const slice<string>& scopes) {
    return strings::Join(std::span<const string>(scopes.data(), scopes.size()), " ");
}

}  // namespace

string Config::AuthCodeURL(std::string_view state, std::span<const AuthCodeOption> opts) const {
    std::string buf(Endpoint.AuthURL.view());
    url::Values v;
    v.Set("response_type", "code");
    v.Set("client_id", ClientID);
    if (!RedirectURL.empty()) v.Set("redirect_uri", RedirectURL);
    if (!Scopes.empty()) v.Set("scope", joinScopes(Scopes));
    if (!state.empty()) v.Set("state", string(state));
    setValues(v, opts);
    buf += strings::Contains(Endpoint.AuthURL.view(), "?") ? '&' : '?';
    buf += v.Encode().view();
    return buf;
}

std::pair<ptr<Token>, error> Config::PasswordCredentialsToken(ptr<context::Context> ctx, std::string_view username,
                                                              std::string_view password) const {
    url::Values v;
    v.Set("grant_type", "password");
    v.Set("username", string(username));
    v.Set("password", string(password));
    if (!Scopes.empty()) v.Set("scope", joinScopes(Scopes));
    return internal::retrieveToken(ctx, *this, v);
}

std::pair<ptr<Token>, error> Config::Exchange(ptr<context::Context> ctx, std::string_view code,
                                              std::span<const AuthCodeOption> opts) const {
    url::Values v;
    v.Set("grant_type", "authorization_code");
    v.Set("code", string(code));
    if (!RedirectURL.empty()) v.Set("redirect_uri", RedirectURL);
    setValues(v, opts);
    return internal::retrieveToken(ctx, *this, v);
}

namespace {

// tokenRefresher: a TokenSource that makes "refresh_token" requests to the
// token endpoint.
struct tokenRefresher final : TokenSource {
    tokenRefresher(ptr<context::Context> ctx, Config conf, string refreshToken)
        : ctx_(std::move(ctx)), conf_(std::move(conf)), refreshToken_(std::move(refreshToken)) {}

    std::pair<ptr<oauth2::Token>, error> Token() override {
        if (refreshToken_.empty()) return {nullptr, errors::New("oauth2: token expired and refresh token is not set")};

        url::Values v;
        v.Set("grant_type", "refresh_token");
        v.Set("refresh_token", refreshToken_);
        auto [tk, err] = internal::retrieveToken(ctx_, conf_, v);
        if (err) return {nullptr, err};
        if (refreshToken_ != tk->RefreshToken) refreshToken_ = tk->RefreshToken;
        return {tk, nullptr};
    }

private:
    ptr<context::Context> ctx_;  // used to get HTTP requests
    Config conf_;
    string refreshToken_;
};

// reuseTokenSource: a TokenSource that holds a single token in memory and
// validates its expiry before each call to retrieve it with Token. If it's
// expired, it will be auto-refreshed using the new TokenSource.
struct reuseTokenSource final : TokenSource {
    reuseTokenSource(ptr<oauth2::Token> t, ptr<oauth2::TokenSource> src, time::Duration expiryDelta = {})
        : new_(std::move(src)), t_(std::move(t)), expiryDelta_(expiryDelta) {}

    std::pair<ptr<oauth2::Token>, error> Token() override {
        std::lock_guard lock(mu_);
        if (t_ && t_->Valid()) return {t_, nullptr};
        auto [t, err] = new_->Token();
        if (err) return {nullptr, err};
        if (!t) return {nullptr, errors::New("oauth2: token source returned no token")};
        internal::tokenAccess::setExpiryDelta(*t, expiryDelta_);
        t_ = t;
        return {t, nullptr};
    }

    ptr<oauth2::TokenSource> new_;  // called when t is expired
    sync::Mutex mu_;                // guards t_ and expiryDelta_
    ptr<oauth2::Token> t_;
    time::Duration expiryDelta_;
};

// staticTokenSource: a TokenSource that always returns the same Token.
struct staticTokenSource final : TokenSource {
    explicit staticTokenSource(ptr<oauth2::Token> t) : t_(std::move(t)) {}
    std::pair<ptr<oauth2::Token>, error> Token() override { return {t_, nullptr}; }
    ptr<oauth2::Token> t_;
};

}  // namespace

ptr<TokenSource> Config::TokenSource(ptr<context::Context> ctx, ptr<Token> t) const {
    authStyleCache.Get();  // so that the refresher's copy of the Config shares it
    auto tkr = make_ptr<tokenRefresher>(std::move(ctx), *this, t ? t->RefreshToken : string());
    return make_ptr<reuseTokenSource>(std::move(t), std::move(tkr));
}

http::Client Config::Client(ptr<context::Context> ctx, ptr<Token> t) const {
    return NewClient(ctx, TokenSource(ctx, std::move(t)));
}

ptr<TokenSource> StaticTokenSource(ptr<Token> t) { return make_ptr<staticTokenSource>(std::move(t)); }

http::Client NewClient(ptr<context::Context> ctx, ptr<TokenSource> src) {
    http::Client cc = internal::ContextClient(ctx);
    if (!src) return cc;
    auto tr = make_ptr<Transport>();
    tr->Base = cc.Transport;
    tr->Source = ReuseTokenSource(nullptr, std::move(src));
    cc.Transport = std::move(tr);
    return cc;
}

ptr<TokenSource> ReuseTokenSource(ptr<Token> t, ptr<TokenSource> src) {
    // Don't wrap a reuseTokenSource in itself. That would work, but cause an
    // unnecessary number of mutex operations. Just build the equivalent one.
    if (auto rt = dynamic_pointer_cast<reuseTokenSource>(src)) {
        if (!t) return rt;
        src = rt->new_;
    }
    return make_ptr<reuseTokenSource>(std::move(t), std::move(src));
}

ptr<TokenSource> ReuseTokenSourceWithExpiry(ptr<Token> t, ptr<TokenSource> src, time::Duration earlyExpiry) {
    if (auto rt = dynamic_pointer_cast<reuseTokenSource>(src)) {
        if (!t) {
            std::lock_guard lock(rt->mu_);
            rt->expiryDelta_ = earlyExpiry;
            return rt;
        }
        src = rt->new_;
    }
    if (t) internal::tokenAccess::setExpiryDelta(*t, earlyExpiry);
    return make_ptr<reuseTokenSource>(std::move(t), std::move(src), earlyExpiry);
}

}  // namespace gopp::oauth2
