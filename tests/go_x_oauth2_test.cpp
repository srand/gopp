// Ported from golang.org/x/oauth2 (v0.36.0; BSD-3-Clause, see
// x/oauth2/LICENSE): oauth2_test.go, token_test.go, deviceauth_test.go and
// the TestTransport tests of transport_test.go, with the names of their
// tests: TestAuthCodeURL, _CustomParam and _Optional, TestURLUnsafeClientConfig,
// TestExchangeRequest and its _CustomParam, _JSONResponse,
// _JSONResponse_Expiry, _BadResponse, _BadResponseType and _NonBasicAuth,
// TestExtraValueRetrieval, TestPasswordCredentialsTokenRequest,
// TestTokenRefreshRequest, TestFetchWithNoRefreshToken, TestTokenRetrieveError
// and TestTokenRetrieveError200, TestRefreshToken_RefreshTokenReplacement and
// _RefreshTokenPreservation, TestConfigClientWithToken; TestTokenExtra,
// TestTokenExpiry and TestTokenTypeMethod; TestDeviceAuthResponseMarshalJson,
// TestDeviceAuthResponseUnmarshalJson and TestDeviceAuthTokenRetrieveError;
// TestTransportNilTokenSource and TestTransportCloseRequestBody(Success).
// Added: DeviceAccessTokenPolls (and its errors), PKCERoundTrip,
// TransportSetsBearer, ReuseTokenSource, and the token endpoint's untrusted
// responses (MalformedTokenResponses, TokenResponseBodyLimit).
//
// Adapted: Go's Token{raw: values} is a form-encoded response from a server
// (TestExtraValueRetrieval), and Go's Extra numbers are int64 or float64, where
// they are all doubles in a json::Value. Go's http.Client has CheckRedirect
// and Jar, which go++'s hasn't, and Config::Client returns it by value.
#include "oauth2_internal.hpp"

#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/oauth2.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace gopp;
namespace oa = gopp::oauth2;

namespace {

std::string S(const string& s) { return std::string(s.view()); }

// A test server, closed at the end of the test.
struct server {
    explicit server(http::handlerFn f) : ts(httptest::NewServer(make_ptr<http::HandlerFunc>(std::move(f)))) {}
    ~server() { ts->Close(); }
    std::string URL() const { return S(ts->URL); }
    ptr<httptest::Server> ts;
};

std::string readBody(http::Request& r) {
    auto [body, err] = io::ReadAll(*r.Body);
    EXPECT_FALSE(err) << "Failed reading request body: " << err;
    return std::string(asStringView(body));
}

std::string requestURL(http::Request& r) { return S(r.URL.String()); }

void respond(http::ResponseWriter& w, std::string_view contentType, std::string_view body, int status = 200) {
    w.Header().Set("Content-Type", string(contentType));
    if (status != 200) w.WriteHeader(status);
    io::WriteString(w, body);
}

oa::Config newConf(const std::string& url) {
    return oa::Config{
        .ClientID = "CLIENT_ID",
        .ClientSecret = "CLIENT_SECRET",
        .Endpoint = {.AuthURL = string(url + "/auth"),
                     .DeviceAuthURL = string(url + "/device"),
                     .TokenURL = string(url + "/token")},
        .RedirectURL = "REDIRECT_URL",
        .Scopes = {"scope1", "scope2"},
    };
}

ptr<context::Context> bg() { return context::Background(); }

struct mockTransport final : http::RoundTripper {
    std::function<std::pair<ptr<http::Response>, error>(http::Request&)> rt;
    std::pair<ptr<http::Response>, error> RoundTrip(http::Request& req) override { return rt(req); }
};

const oa::RetrieveError* asRetrieveError(const error& err) {
    return dynamic_cast<const oa::RetrieveError*>(err.get());
}

const std::string tokenBody =
    "access_token=90d64460d14870c08c81352a05dedd3465940a7c&scope=user&token_type=bearer";

}  // namespace

TEST(GoOAuth2, AuthCodeURL) {
    auto conf = newConf("server");
    auto url = conf.AuthCodeURL("foo", std::array{oa::AccessTypeOffline, oa::ApprovalForce});
    EXPECT_EQ(S(url),
              "server/auth?access_type=offline&client_id=CLIENT_ID&prompt=consent&redirect_uri=REDIRECT_URL&"
              "response_type=code&scope=scope1+scope2&state=foo");
}

TEST(GoOAuth2, AuthCodeURL_CustomParam) {
    auto conf = newConf("server");
    auto url = conf.AuthCodeURL("baz", std::array{oa::SetAuthURLParam("foo", "bar")});
    EXPECT_EQ(S(url),
              "server/auth?client_id=CLIENT_ID&foo=bar&redirect_uri=REDIRECT_URL&response_type=code&"
              "scope=scope1+scope2&state=baz");
}

TEST(GoOAuth2, AuthCodeURL_Optional) {
    oa::Config conf{.ClientID = "CLIENT_ID", .Endpoint = {.AuthURL = "/auth-url", .TokenURL = "/token-url"}};
    EXPECT_EQ(S(conf.AuthCodeURL("")), "/auth-url?client_id=CLIENT_ID&response_type=code");
}

TEST(GoOAuth2, AuthCodeURL_QueryInAuthURL) {
    oa::Config conf{.ClientID = "id", .Endpoint = {.AuthURL = "/auth?tenant=a"}};
    EXPECT_EQ(S(conf.AuthCodeURL("s")), "/auth?tenant=a&client_id=id&response_type=code&state=s");
}

TEST(GoOAuth2, URLUnsafeClientConfig) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Basic Q0xJRU5UX0lEJTNGJTNGOkNMSUVOVF9TRUNSRVQlM0YlM0Y=");
        respond(w, "application/x-www-form-urlencoded", tokenBody);
    });
    auto conf = newConf(ts.URL());
    conf.ClientID = "CLIENT_ID??";
    conf.ClientSecret = "CLIENT_SECRET??";
    auto [tok, err] = conf.Exchange(bg(), "exchange-code");
    EXPECT_FALSE(err) << err;
}

TEST(GoOAuth2, ExchangeRequest) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Basic Q0xJRU5UX0lEOkNMSUVOVF9TRUNSRVQ=");
        EXPECT_EQ(S(r.Header.Get("Content-Type")), "application/x-www-form-urlencoded");
        EXPECT_EQ(readBody(r), "code=exchange-code&grant_type=authorization_code&redirect_uri=REDIRECT_URL");
        respond(w, "application/x-www-form-urlencoded", tokenBody);
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.Exchange(bg(), "exchange-code");
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(tok->Valid());
    EXPECT_EQ(S(tok->AccessToken), "90d64460d14870c08c81352a05dedd3465940a7c");
    EXPECT_EQ(S(tok->TokenType), "bearer");
    EXPECT_EQ(tok->Extra("scope"), json::Value("user"));
}

TEST(GoOAuth2, ExchangeRequest_CustomParam) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Basic Q0xJRU5UX0lEOkNMSUVOVF9TRUNSRVQ=");
        EXPECT_EQ(S(r.Header.Get("Content-Type")), "application/x-www-form-urlencoded");
        EXPECT_EQ(readBody(r), "code=exchange-code&foo=bar&grant_type=authorization_code&redirect_uri=REDIRECT_URL");
        respond(w, "application/x-www-form-urlencoded", tokenBody);
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.Exchange(bg(), "exchange-code", std::array{oa::SetAuthURLParam("foo", "bar")});
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(tok->Valid());
    EXPECT_EQ(S(tok->AccessToken), "90d64460d14870c08c81352a05dedd3465940a7c");
    EXPECT_EQ(S(tok->TokenType), "bearer");
    EXPECT_EQ(tok->Extra("scope"), json::Value("user"));
}

TEST(GoOAuth2, ExchangeRequest_JSONResponse) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Basic Q0xJRU5UX0lEOkNMSUVOVF9TRUNSRVQ=");
        EXPECT_EQ(readBody(r), "code=exchange-code&grant_type=authorization_code&redirect_uri=REDIRECT_URL");
        respond(w, "application/json",
                R"({"access_token": "90d64460d14870c08c81352a05dedd3465940a7c", "scope": "user", "token_type": "bearer", "expires_in": 86400})");
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.Exchange(bg(), "exchange-code");
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(tok->Valid());
    EXPECT_EQ(S(tok->AccessToken), "90d64460d14870c08c81352a05dedd3465940a7c");
    EXPECT_EQ(S(tok->TokenType), "bearer");
    EXPECT_EQ(tok->ExpiresIn, 86400);
    EXPECT_EQ(tok->Extra("scope"), json::Value("user"));
    EXPECT_EQ(tok->Extra("expires_in"), json::Value(86400.0));
}

TEST(GoOAuth2, ExtraValueRetrieval) {
    // A form-encoded response is what Go's Token{raw: url.Values} is.
    server ts([](http::ResponseWriter& w, http::Request&) {
        url::Values values;
        values.Set("access_token", "a");
        values.Set("scope", "user");
        values.Set("token_type", "bearer");
        values.Set("expires_in", "86400.92");
        values.Set("server_time", "1443571905.5606415");
        values.Set("referer_ip", "10.0.0.1");
        values.Set("etag", "\"afZYj912P4alikMz_P11982\"");
        values.Set("request_id", "86400");
        values.Set("untrimmed", "  untrimmed  ");
        respond(w, "application/x-www-form-urlencoded", S(values.Encode()));
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.Exchange(bg(), "code");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(tok->Extra("scope"), json::Value("user"));
    EXPECT_EQ(tok->Extra("server_time"), json::Value(1443571905.5606415));
    EXPECT_EQ(tok->Extra("referer_ip"), json::Value("10.0.0.1"));
    EXPECT_EQ(tok->Extra("expires_in"), json::Value(86400.92));
    EXPECT_EQ(tok->Extra("request_id"), json::Value(86400));
    EXPECT_EQ(tok->Extra("untrimmed"), json::Value("  untrimmed  "));
    EXPECT_EQ(tok->Extra("etag"), json::Value("\"afZYj912P4alikMz_P11982\""));
    EXPECT_EQ(tok->Extra("absent"), json::Value(""));  // Go's url.Values.Get of a missing key
    // The form's expires_in is in seconds from now.
    EXPECT_EQ(tok->ExpiresIn, 0);
}

TEST(GoOAuth2, ExchangeRequest_JSONResponse_Expiry) {
    const int seconds = 24 * 60 * 60;
    struct tc {
        const char* name;
        std::string expires;
        bool want;
        bool nullExpires;
    };
    for (const auto& c : std::vector<tc>{
             {"normal", R"("expires_in": 86400)", true, false},
             {"paypal", R"("expires_in": "86400")", true, false},
             {"issue_239", R"("expires_in": null)", true, true},
             {"wrong_type", R"("expires_in": false)", false, false},
             {"wrong_type2", R"("expires_in": {})", false, false},
             {"wrong_value", R"("expires_in": "zzz")", false, false},
         }) {
        SCOPED_TRACE(c.name);
        server ts([&](http::ResponseWriter& w, http::Request&) {
            respond(w, "application/json",
                    R"({"access_token": "90d", "scope": "user", "token_type": "bearer", )" + c.expires + "}");
        });
        auto conf = newConf(ts.URL());
        auto t1 = time::Now().Add(seconds * time::Second);
        auto [tok, err] = conf.Exchange(bg(), "exchange-code");
        auto t2 = time::Now().Add(seconds * time::Second);
        EXPECT_EQ(!err, c.want) << err;
        if (!c.want) continue;
        ASSERT_TRUE(tok->Valid());
        if (c.nullExpires && tok->Expiry.IsZero()) continue;
        EXPECT_FALSE(tok->Expiry.Before(t1) || tok->Expiry.After(t2))
            << "Expiry " << tok->Expiry.String() << " should be between " << t1.String() << " and " << t2.String();
    }
}

TEST(GoOAuth2, ExchangeRequest_BadResponse) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json", R"({"scope": "user", "token_type": "bearer"})");
    });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "code");
    ASSERT_TRUE(err) << "expected error from missing access_token";
    EXPECT_EQ(S(err.Error()), "oauth2: server response missing access_token");
}

TEST(GoOAuth2, ExchangeRequest_BadResponseType) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json", R"({"access_token":123,  "scope": "user", "token_type": "bearer"})");
    });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "exchange-code");
    EXPECT_TRUE(err) << "expected error from non-string access_token";
}

TEST(GoOAuth2, ExchangeRequest_NonBasicAuth) {
    int calls = 0;
    auto tr = make_ptr<mockTransport>();
    tr->rt = [&](http::Request& r) -> std::pair<ptr<http::Response>, error> {
        calls++;
        EXPECT_EQ(S(r.Header.Get("Authorization")), "");
        return {nullptr, errors::New("no response")};
    };
    oa::Config conf{.ClientID = "CLIENT_ID",
                    .Endpoint = {.AuthURL = "https://accounts.google.com/auth",
                                 .TokenURL = "https://accounts.google.com/token",
                                 .AuthStyle = oa::AuthStyle::InParams}};
    auto ctx = oa::WithClient(bg(), http::Client{.Transport = tr});
    auto [tok, err] = conf.Exchange(ctx, "code");
    EXPECT_TRUE(err);
    EXPECT_EQ(calls, 1);  // an explicit style is not probed
}

TEST(GoOAuth2, PasswordCredentialsTokenRequest) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Basic Q0xJRU5UX0lEOkNMSUVOVF9TRUNSRVQ=");
        EXPECT_EQ(S(r.Header.Get("Content-Type")), "application/x-www-form-urlencoded");
        EXPECT_EQ(readBody(r), "grant_type=password&password=password1&scope=scope1+scope2&username=user1");
        respond(w, "application/x-www-form-urlencoded", tokenBody);
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.PasswordCredentialsToken(bg(), "user1", "password1");
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(tok->Valid());
    EXPECT_EQ(S(tok->AccessToken), "90d64460d14870c08c81352a05dedd3465940a7c");
    EXPECT_EQ(S(tok->TokenType), "bearer");
}

TEST(GoOAuth2, TokenRefreshRequest) {
    int refreshes = 0;
    server ts([&](http::ResponseWriter& w, http::Request& r) {
        if (requestURL(r) == "/somethingelse") return;
        refreshes++;
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(S(r.Header.Get("Content-Type")), "application/x-www-form-urlencoded");
        EXPECT_EQ(readBody(r), "grant_type=refresh_token&refresh_token=REFRESH_TOKEN");
        respond(w, "application/json", R"({"access_token": "foo", "refresh_token": "bar"})");
    });
    auto conf = newConf(ts.URL());
    auto tok = make_ptr<oa::Token>();
    tok->RefreshToken = "REFRESH_TOKEN";
    auto c = conf.Client(bg(), tok);
    auto [resp, err] = c.Get(ts.URL() + "/somethingelse");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(refreshes, 1);
}

TEST(GoOAuth2, FetchWithNoRefreshToken) {
    server ts([](http::ResponseWriter&, http::Request& r) {
        if (requestURL(r) == "/somethingelse") return;
        ADD_FAILURE() << "the token endpoint was called: " << requestURL(r);
    });
    auto conf = newConf(ts.URL());
    auto c = conf.Client(bg(), nullptr);
    auto [resp, err] = c.Get(ts.URL() + "/somethingelse");
    EXPECT_TRUE(err) << "Fetch should return an error if no refresh token is set";
    EXPECT_NE(S(err.Error()).find("oauth2: token expired and refresh token is not set"), std::string::npos) << err;
}

TEST(GoOAuth2, TokenRetrieveError) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/token");
        respond(w, "application/json", R"({"error": "invalid_grant"})", 400);
    });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "exchange-code");
    ASSERT_TRUE(err);
    auto* re = asRetrieveError(err);
    ASSERT_NE(re, nullptr) << "expected a RetrieveError; error was: " << err;
    EXPECT_EQ(S(err.Error()), R"(oauth2: "invalid_grant")");
    EXPECT_EQ(S(re->ErrorCode), "invalid_grant");
    EXPECT_EQ(re->Response->StatusCode, 400);
    EXPECT_EQ(std::string(asStringView(re->Body)), R"({"error": "invalid_grant"})");
}

// An unorthodox server that returns 200 in the error case.
TEST(GoOAuth2, TokenRetrieveError200) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json", R"({"error": "invalid_grant"})");
    });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "exchange-code");
    ASSERT_TRUE(err);
    auto* re = asRetrieveError(err);
    ASSERT_NE(re, nullptr) << "expected a RetrieveError; error was: " << err;
    EXPECT_EQ(S(err.Error()), R"(oauth2: "invalid_grant")");
    EXPECT_EQ(S(re->ErrorCode), "invalid_grant");
}

TEST(GoOAuth2, TokenRetrieveErrorWithoutCode) {
    server ts([](http::ResponseWriter& w, http::Request&) { respond(w, "text/html", "<b>gateway</b>", 502); });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "exchange-code");
    auto* re = asRetrieveError(err);
    ASSERT_NE(re, nullptr) << err;
    EXPECT_EQ(S(err.Error()), "oauth2: cannot fetch token: 502 Bad Gateway\nResponse: <b>gateway</b>");
}

TEST(GoOAuth2, RefreshToken_RefreshTokenReplacement) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json",
                R"({"access_token":"ACCESS_TOKEN",  "scope": "user", "token_type": "bearer", "refresh_token": "NEW_REFRESH_TOKEN"})");
    });
    auto old = make_ptr<oa::Token>();
    old->RefreshToken = "OLD_REFRESH_TOKEN";
    auto tkr = newConf(ts.URL()).TokenSource(bg(), old);
    auto [tk, err] = tkr->Token();
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(S(tk->RefreshToken), "NEW_REFRESH_TOKEN");
}

TEST(GoOAuth2, RefreshToken_RefreshTokenPreservation) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json", R"({"access_token":"ACCESS_TOKEN",  "scope": "user", "token_type": "bearer"})");
    });
    auto old = make_ptr<oa::Token>();
    old->RefreshToken = "OLD_REFRESH_TOKEN";
    auto tkr = newConf(ts.URL()).TokenSource(bg(), old);
    auto [tk, err] = tkr->Token();
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(S(tk->RefreshToken), "OLD_REFRESH_TOKEN");
}

TEST(GoOAuth2, ConfigClientWithToken) {
    auto tok = make_ptr<oa::Token>();
    tok->AccessToken = "abc123";
    server ts([&](http::ResponseWriter&, http::Request& r) {
        EXPECT_EQ(S(r.Header.Get("Authorization")), "Bearer abc123");
    });
    auto c = newConf(ts.URL()).Client(bg(), tok);
    auto [resp, err] = c.Get(ts.URL());
    EXPECT_FALSE(err) << err;
}

TEST(GoOAuth2, ReuseTokenSource) {
    struct counting final : oa::TokenSource {
        int calls = 0;
        std::pair<ptr<oa::Token>, error> Token() override {
            auto t = make_ptr<oa::Token>();
            t->AccessToken = string("t" + std::to_string(++calls));
            t->Expiry = time::Now().Add(time::Hour);
            return {t, nullptr};
        }
    };
    auto inner = make_ptr<counting>();
    auto src = oa::ReuseTokenSource(nullptr, inner);
    auto [a, e1] = src->Token();
    auto [b, e2] = src->Token();
    EXPECT_EQ(S(a->AccessToken), "t1");
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(inner->calls, 1);
    // Reusing a reuse source with no token is itself.
    EXPECT_EQ(oa::ReuseTokenSource(nullptr, src).get(), src.get());

    // An expired token is replaced.
    auto old = make_ptr<oa::Token>();
    old->AccessToken = "old";
    old->Expiry = time::Now().Add(-time::Hour);
    auto src2 = oa::ReuseTokenSource(old, inner);
    auto [c, e3] = src2->Token();
    EXPECT_EQ(S(c->AccessToken), "t2");

    // The expiry delta of ReuseTokenSourceWithExpiry: valid for an hour, but
    // not within two hours of its expiry.
    auto src3 = oa::ReuseTokenSourceWithExpiry(nullptr, inner, 2 * time::Hour);
    auto [d, e4] = src3->Token();
    auto [e, e5] = src3->Token();
    EXPECT_EQ(S(d->AccessToken), "t3");
    EXPECT_EQ(S(e->AccessToken), "t4");

    auto st = oa::StaticTokenSource(old);
    auto [f, e6] = st->Token();
    EXPECT_EQ(f.get(), old.get());
}

// ---------------------------------------------------------------------------
// token_test.go

TEST(GoOAuth2, TokenExtra) {
    struct tc {
        std::string key;
        json::Value val, want;
    };
    const std::string key = "extra-key";
    for (const auto& c : std::vector<tc>{
             {key, "abc", "abc"},
             {key, 123, 123},
             {key, "", ""},
             {"other-key", "def", nullptr},
         }) {
        json::Value::object extra;
        extra[string(c.key)] = c.val;
        auto tok = make_ptr<oa::Token>()->WithExtra(json::Value(extra));
        EXPECT_EQ(tok->Extra(key), c.want) << c.key;
    }
    // Without extra values, or with something else than an object.
    EXPECT_TRUE(oa::Token().Extra("a").IsNull());
    EXPECT_TRUE(make_ptr<oa::Token>()->WithExtra(json::Value("x"))->Extra("a").IsNull());
}

TEST(GoOAuth2, TokenExpiry) {
    auto now = time::Now();
    oa::internal::timeNow = [now] { return now; };
    struct reset {
        ~reset() { oa::internal::timeNow = [] { return time::Now(); }; }
    } r;

    const auto defaultExpiryDelta = 10 * time::Second;
    struct tc {
        const char* name;
        time::Time expiry;
        time::Duration delta;
        bool want;
    };
    for (const auto& c : std::vector<tc>{
             {"12 seconds", now.Add(12 * time::Second), {}, false},
             {"10 seconds", now.Add(defaultExpiryDelta), {}, false},
             {"10 seconds-1ns", now.Add(defaultExpiryDelta - time::Nanosecond), {}, true},
             {"-1 hour", now.Add(-time::Hour), {}, true},
             {"12 seconds, custom expiryDelta", now.Add(12 * time::Second), 5 * time::Second, false},
             {"5 seconds, custom expiryDelta", now.Add(5 * time::Second), 5 * time::Second, false},
             {"5 seconds-1ns, custom expiryDelta", now.Add(5 * time::Second - time::Nanosecond), 5 * time::Second, true},
             {"-1 hour, custom expiryDelta", now.Add(-time::Hour), 5 * time::Second, true},
         }) {
        oa::Token tok;
        tok.Expiry = c.expiry;
        oa::internal::tokenAccess::setExpiryDelta(tok, c.delta);
        EXPECT_EQ(oa::internal::tokenAccess::expired(tok), c.want) << c.name;
    }
    // Valid needs an access token, and no expiry never expires.
    oa::Token noAccess;
    EXPECT_FALSE(noAccess.Valid());
    oa::Token forever;
    forever.AccessToken = "a";
    EXPECT_TRUE(forever.Valid());
}

TEST(GoOAuth2, TokenTypeMethod) {
    for (auto [name, typ, want] : std::vector<std::array<const char*, 3>>{
             {"bearer-mixed_case", "beAREr", "Bearer"},
             {"default-bearer", "", "Bearer"},
             {"basic", "basic", "Basic"},
             {"basic-capitalized", "Basic", "Basic"},
             {"mac", "mac", "MAC"},
             {"mac-caps", "MAC", "MAC"},
             {"mac-mixed_case", "mAc", "MAC"},
             {"other", "DPoP", "DPoP"},
         }) {
        oa::Token tok;
        tok.TokenType = typ;
        EXPECT_EQ(S(tok.Type()), want) << name;
    }
}

// ---------------------------------------------------------------------------
// deviceauth_test.go

TEST(GoOAuth2, DeviceAuthResponseMarshalJson) {
    {
        auto [b, err] = json::Marshal(oa::DeviceAuthResponse{});
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(std::string(asStringView(b)), R"({"device_code":"","user_code":"","verification_uri":""})");
    }
    {
        auto begin = time::Now();
        oa::DeviceAuthResponse r;
        r.Expiry = time::Now().Add(100 * time::Second + 999 * time::Millisecond);
        auto [b, err] = json::Marshal(r);
        ASSERT_FALSE(err) << err;
        if (time::Since(begin) > 999 * time::Millisecond) GTEST_SKIP() << "test ran too slowly to compare expires_in";
        EXPECT_EQ(std::string(asStringView(b)),
                  R"({"expires_in":100,"device_code":"","user_code":"","verification_uri":""})");
    }
    {
        oa::DeviceAuthResponse r;
        r.DeviceCode = "d";
        r.UserCode = "u";
        r.VerificationURI = "https://v";
        r.VerificationURIComplete = "https://v?c=u";
        r.Interval = 7;
        auto [b, err] = json::Marshal(r);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(std::string(asStringView(b)),
                  R"({"device_code":"d","user_code":"u","verification_uri":"https://v","verification_uri_complete":"https://v?c=u","interval":7})");
    }
}

TEST(GoOAuth2, DeviceAuthResponseUnmarshalJson) {
    {
        oa::DeviceAuthResponse got;
        ASSERT_FALSE(json::Unmarshal(std::string_view("{}"), &got));
        EXPECT_TRUE(got.Expiry.IsZero());
        EXPECT_EQ(S(got.DeviceCode), "");
    }
    {
        auto begin = time::Now();
        oa::DeviceAuthResponse got;
        ASSERT_FALSE(json::Unmarshal(std::string_view(R"({"expires_in":100})"), &got));
        auto want = time::Now().UTC().Add(100 * time::Second);
        auto margin = time::Second + time::Since(begin);
        auto diff = got.Expiry.Sub(want).Abs();
        EXPECT_LE(diff, margin) << "expiry time difference too large, got " << got.Expiry.String();
    }
    {
        oa::DeviceAuthResponse got;
        ASSERT_FALSE(json::Unmarshal(
            std::string_view(R"({"device_code":"d","user_code":"u","verification_url":"https://g","interval":3})"),
            &got));
        EXPECT_EQ(S(got.DeviceCode), "d");
        EXPECT_EQ(S(got.UserCode), "u");
        EXPECT_EQ(S(got.VerificationURI), "https://g");  // Google's name for it
        EXPECT_EQ(got.Interval, 3);
        ASSERT_FALSE(json::Unmarshal(std::string_view(R"({"verification_uri":"https://a","verification_url":"b"})"), &got));
        EXPECT_EQ(S(got.VerificationURI), "https://a");
        EXPECT_TRUE(json::Unmarshal(std::string_view(R"({"expires_in":"x"})"), &got));
    }
}

TEST(GoOAuth2, DeviceAuthTokenRetrieveError) {
    for (auto [name, contentType, body] : std::vector<std::array<const char*, 3>>{
             {"UrlEncoding", "application/x-www-form-urlencoded", "error=invalid_grant&error_description=sometext"},
             {"JSON", "application/json", R"({"error": "invalid_grant", "error_description": "sometext"})"},
         }) {
        SCOPED_TRACE(name);
        server ts([&](http::ResponseWriter& w, http::Request& r) {
            EXPECT_EQ(requestURL(r), "/device");
            respond(w, contentType, body, 400);
        });
        auto [da, err] = newConf(ts.URL()).DeviceAuth(bg());
        ASSERT_TRUE(err);
        auto* re = asRetrieveError(err);
        ASSERT_NE(re, nullptr) << "expected a RetrieveError; error was: " << err;
        EXPECT_EQ(S(err.Error()), R"(oauth2: "invalid_grant" "sometext")");
        EXPECT_EQ(S(re->ErrorCode), "invalid_grant");
        EXPECT_EQ(S(re->ErrorDescription), "sometext");
    }
}

TEST(GoOAuth2, DeviceAuth) {
    server ts([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(requestURL(r), "/device");
        EXPECT_EQ(S(r.Header.Get("Accept")), "application/json");
        EXPECT_EQ(readBody(r), "client_id=CLIENT_ID&foo=bar&scope=scope1+scope2");
        respond(w, "application/json",
                R"({"device_code":"DC","user_code":"WDJB-MJHT","verification_uri":"https://v","expires_in":600,"interval":2})");
    });
    auto begin = time::Now();
    auto [da, err] = newConf(ts.URL()).DeviceAuth(bg(), std::array{oa::SetAuthURLParam("foo", "bar")});
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(S(da->DeviceCode), "DC");
    EXPECT_EQ(S(da->UserCode), "WDJB-MJHT");
    EXPECT_EQ(da->Interval, 2);
    EXPECT_TRUE(da->Expiry.After(begin.Add(590 * time::Second)));
    EXPECT_TRUE(da->Expiry.Before(begin.Add(601 * time::Second)));

    oa::Config noDevice;
    auto [da2, err2] = noDevice.DeviceAuth(bg());
    ASSERT_TRUE(err2);
    EXPECT_EQ(S(err2.Error()), "endpoint missing DeviceAuthURL");

    server bad([](http::ResponseWriter& w, http::Request&) { respond(w, "application/json", "{nope"); });
    auto [da3, err3] = newConf(bad.URL()).DeviceAuth(bg());
    ASSERT_TRUE(err3);
    EXPECT_EQ(S(err3.Error()).rfind("unmarshal ", 0), 0u) << err3;
}

// The server answers authorization_pending twice, then slow_down, then a
// token: the polls are an interval apart (1 s), and after slow_down, 5 s
// more apart.
TEST(GoOAuth2, DeviceAccessTokenPolls) {
    std::vector<time::Time> at;
    server ts([&](http::ResponseWriter& w, http::Request& r) {
        at.push_back(time::Now());
        EXPECT_EQ(requestURL(r), "/token");
        EXPECT_EQ(readBody(r),
                  "client_id=CLIENT_ID&device_code=DC&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
                  "&scope=scope1+scope2");
        switch (at.size()) {
        case 1:
        case 2: respond(w, "application/json", R"({"error":"authorization_pending"})", 400); break;
        case 3: respond(w, "application/json", R"({"error":"slow_down"})", 400); break;
        default:
            respond(w, "application/json", R"({"access_token":"AT","token_type":"Bearer","refresh_token":"RT"})");
        }
    });
    oa::DeviceAuthResponse da;
    da.DeviceCode = "DC";
    da.Interval = 1;
    // With AutoDetect, an error response is retried with the other style (as
    // Go's), which would double the requests.
    auto conf = newConf(ts.URL());
    conf.Endpoint.AuthStyle = oa::AuthStyle::InHeader;
    auto [tok, err] = conf.DeviceAccessToken(bg(), da);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(S(tok->AccessToken), "AT");
    EXPECT_EQ(S(tok->RefreshToken), "RT");
    ASSERT_EQ(at.size(), 4u);
    const auto slack = 50 * time::Millisecond;  // the ticks are exact, the handlers are not
    EXPECT_GE(at[1].Sub(at[0]), time::Second - slack);
    EXPECT_GE(at[2].Sub(at[1]), time::Second - slack);
    EXPECT_GE(at[3].Sub(at[2]), 6 * time::Second - slack);
}

TEST(GoOAuth2, DeviceAccessTokenErrors) {
    // access_denied ends the poll; the ctx, too.
    int calls = 0;
    server ts([&](http::ResponseWriter& w, http::Request&) {
        calls++;
        respond(w, "application/json", R"({"error":"access_denied"})", 400);
    });
    oa::DeviceAuthResponse da;
    da.DeviceCode = "DC";
    da.Interval = 1;
    auto conf = newConf(ts.URL());
    conf.Endpoint.AuthStyle = oa::AuthStyle::InHeader;
    auto [tok, err] = conf.DeviceAccessToken(bg(), da);
    ASSERT_TRUE(err);
    EXPECT_EQ(tok, nullptr);
    ASSERT_NE(asRetrieveError(err), nullptr);
    EXPECT_EQ(S(asRetrieveError(err)->ErrorCode), "access_denied");
    EXPECT_EQ(calls, 1);

    auto [ctx, cancel] = context::WithTimeout(bg(), 100 * time::Millisecond);
    auto [tok2, err2] = conf.DeviceAccessToken(ctx, da);
    cancel();
    EXPECT_TRUE(errors::Is(err2, context::DeadlineExceeded)) << err2;
    EXPECT_EQ(calls, 1);

    // An expired device code ends it: the deadline is da.Expiry.
    da.Expiry = time::Now().Add(100 * time::Millisecond);
    auto [tok3, err3] = conf.DeviceAccessToken(bg(), da);
    EXPECT_TRUE(errors::Is(err3, context::DeadlineExceeded)) << err3;
}

// A hostile interval (negative, zero, or huge) is the default or an hour
// at most, not a panic in the ticker or an overflow: the poll waits, and
// the ctx ends it before the first one.
TEST(GoOAuth2, DeviceAccessTokenHostileInterval) {
    int calls = 0;
    server ts([&](http::ResponseWriter& w, http::Request&) {
        calls++;
        respond(w, "application/json", R"({"access_token":"AT"})");
    });
    auto conf = newConf(ts.URL());
    for (int64 interval : {int64(-1), int64(INT64_MIN), int64(9000000000000000000), int64(INT64_MAX), int64(3601)}) {
        SCOPED_TRACE(interval);
        oa::DeviceAuthResponse da;
        da.DeviceCode = "DC";
        da.Interval = interval;
        auto [ctx, cancel] = context::WithTimeout(bg(), 50 * time::Millisecond);
        auto [tok, err] = conf.DeviceAccessToken(ctx, da);
        cancel();
        EXPECT_EQ(tok, nullptr);
        EXPECT_TRUE(errors::Is(err, context::DeadlineExceeded)) << err;
    }
    EXPECT_EQ(calls, 0);

    // The same, as the JSON of a server.
    oa::DeviceAuthResponse parsed;
    ASSERT_FALSE(json::Unmarshal(std::string_view(R"({"device_code":"DC","interval":-1})"), &parsed));
    auto [ctx, cancel] = context::WithTimeout(bg(), 50 * time::Millisecond);
    auto [tok, err] = conf.DeviceAccessToken(ctx, parsed);
    cancel();
    EXPECT_TRUE(errors::Is(err, context::DeadlineExceeded)) << err;
    EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// PKCE

TEST(GoOAuth2, PKCERoundTrip) {
    // RFC 7636, appendix B.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    EXPECT_EQ(S(oa::S256ChallengeFromVerifier(verifier)), challenge);

    auto v1 = oa::GenerateVerifier();
    auto v2 = oa::GenerateVerifier();
    EXPECT_EQ(v1.size(), 43u);
    EXPECT_NE(S(v1), S(v2));
    for (char c : v1.view()) EXPECT_TRUE(isalnum((unsigned char)c) || c == '-' || c == '_') << c;

    // The authorization URL carries the challenge, and the exchange the
    // verifier that the server checks against it.
    std::string url;
    server ts([&](http::ResponseWriter& w, http::Request& r) {
        auto [form, err] = url::ParseQuery(readBody(r));
        EXPECT_EQ(S(form.Get("code_verifier")), verifier);
        EXPECT_EQ(S(oa::S256ChallengeFromVerifier(form.Get("code_verifier"))), challenge);
        respond(w, "application/json", R"({"access_token":"AT"})");
    });
    auto conf = newConf(ts.URL());
    auto authURL = conf.AuthCodeURL("st", std::array{oa::S256ChallengeOption(verifier)});
    auto [u, perr] = url::Parse(authURL.view());
    ASSERT_FALSE(perr) << perr;
    EXPECT_EQ(S(u.Query().Get("code_challenge")), challenge);
    EXPECT_EQ(S(u.Query().Get("code_challenge_method")), "S256");
    auto [tok, err] = conf.Exchange(bg(), "code", std::array{oa::VerifierOption(verifier)});
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(S(tok->AccessToken), "AT");
}

// ---------------------------------------------------------------------------
// transport_test.go, and the bearer token

TEST(GoOAuth2, TransportSetsBearer) {
    std::vector<std::string> seen;
    server ts([&](http::ResponseWriter&, http::Request& r) { seen.push_back(S(r.Header.Get("Authorization"))); });
    auto tok = make_ptr<oa::Token>();
    tok->AccessToken = "abc";
    auto client = oa::NewClient(bg(), oa::StaticTokenSource(tok));
    auto [req, rerr] = http::NewRequest("GET", ts.URL());
    ASSERT_FALSE(rerr);
    req->Header.Set("Authorization", "stale");
    auto [resp, err] = client.Do(req);
    ASSERT_FALSE(err) << err;
    resp->Body->Close();
    // The request that the caller passed is as it was.
    EXPECT_EQ(S(req->Header.Get("Authorization")), "stale");

    tok->TokenType = "mac";
    auto [resp2, err2] = client.Get(ts.URL());
    ASSERT_FALSE(err2) << err2;
    resp2->Body->Close();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "Bearer abc");
    EXPECT_EQ(seen[1], "MAC abc");

    // SetAuthHeader on its own.
    auto [r3, e3] = http::NewRequest("GET", "http://x/");
    tok->TokenType = "";
    tok->SetAuthHeader(*r3);
    EXPECT_EQ(S(r3->Header.Get("Authorization")), "Bearer abc");

    // A null source is the context client, without a token.
    auto plain = oa::NewClient(bg(), nullptr);
    auto [resp3, err3] = plain.Get(ts.URL());
    ASSERT_FALSE(err3) << err3;
    resp3->Body->Close();
    EXPECT_EQ(seen[2], "");
}

TEST(GoOAuth2, TransportNilTokenSource) {
    server ts([](http::ResponseWriter&, http::Request&) {});
    auto tr = make_ptr<oa::Transport>();
    http::Client client{.Transport = tr};
    auto [resp, err] = client.Get(ts.URL());
    EXPECT_TRUE(err) << "want an error with nil token source";
    EXPECT_EQ(resp, nullptr);
}

namespace {
struct readCloseCounter final : io::ReadCloser {
    int CloseCount = 0;
    error ReadErr;
    std::pair<isize, error> Read(std::span<byte>) override { return {0, ReadErr}; }
    error Close() override {
        CloseCount++;
        return nullptr;
    }
};
}  // namespace

TEST(GoOAuth2, TransportCloseRequestBody) {
    server ts([](http::ResponseWriter&, http::Request&) {});
    auto tr = make_ptr<oa::Transport>();
    http::Client client{.Transport = tr};
    auto body = make_ptr<readCloseCounter>();
    body->ReadErr = errors::New("readCloseCounter.Read not implemented");
    auto [resp, err] = client.Post(ts.URL(), "application/json", body);
    EXPECT_TRUE(err);
    EXPECT_EQ(resp, nullptr);
    EXPECT_EQ(body->CloseCount, 1) << "the body is closed once";
}

TEST(GoOAuth2, TransportCloseRequestBodySuccess) {
    server ts([](http::ResponseWriter&, http::Request&) {});
    auto tok = make_ptr<oa::Token>();
    tok->AccessToken = "abc";
    auto tr = make_ptr<oa::Transport>();
    tr->Source = oa::StaticTokenSource(tok);
    http::Client client{.Transport = tr};
    auto body = make_ptr<readCloseCounter>();
    body->ReadErr = io::EOF_;
    auto [resp, err] = client.Post(ts.URL(), "application/json", body);
    EXPECT_FALSE(err) << err;
    ASSERT_NE(resp, nullptr);
    resp->Body->Close();
    EXPECT_EQ(body->CloseCount, 1) << "the body is closed once";
}

// ---------------------------------------------------------------------------
// Untrusted responses

TEST(GoOAuth2, MalformedTokenResponses) {
    struct tc {
        const char* name;
        const char* contentType;
        std::string body;
        int status;
        const char* wantErr;  // a part of the message; "" for none
    };
    for (const auto& c : std::vector<tc>{
             {"truncated json", "application/json", R"({"access_token": "a)", 200, "oauth2: cannot parse json: "},
             {"json array", "application/json", "[]", 200, "oauth2: cannot parse json: "},
             {"json string", "application/json", "\"x\"", 200, "oauth2: cannot parse json: "},
             {"json null", "application/json", "null", 200, "oauth2: server response missing access_token"},
             {"empty", "application/json", "", 200, "oauth2: cannot parse json: "},
             {"binary", "application/octet-stream", std::string("\xff\x00\x01{", 4), 200, "oauth2: cannot parse json: "},
             {"html", "text/html", "access_token=a", 200, "oauth2: cannot parse json: "},
             {"bad form", "application/x-www-form-urlencoded", "access_token=%zz", 200,
              "oauth2: cannot parse response: "},
             {"empty form", "application/x-www-form-urlencoded", "", 200, "oauth2: server response missing access_token"},
             {"error status, bad json", "application/json", "{{{", 500, "oauth2: cannot fetch token: 500"},
             {"error status, bad form", "text/plain", "a=%zz", 500, "oauth2: cannot fetch token: 500"},
             {"error code in form", "text/plain", "error=bad&error_uri=http%3A%2F%2Fe", 200, "oauth2: \"bad\" \"http://e\""},
             {"expires_in too big", "application/json", R"({"access_token":"a","expires_in":9223372036854775807})", 200,
              ""},
             {"expires_in out of int64", "application/json", R"({"access_token":"a","expires_in":99999999999999999999})",
              200, "oauth2: cannot parse json: "},
             {"expires_in fractional", "application/json", R"({"access_token":"a","expires_in":1.5})", 200,
              "oauth2: cannot parse json: "},
             {"expires_in negative", "application/json", R"({"access_token":"a","expires_in":-5})", 200, ""},
             {"expires_in very negative", "application/json", R"({"access_token":"a","expires_in":-9223372036854775807})",
              200, ""},
             {"form expires_in huge", "text/plain", "access_token=a&expires_in=99999999999999999999", 200, ""},
             {"form expires_in negative", "text/plain", "access_token=a&expires_in=-99999999999999999999", 200, ""},
             {"form expires_in junk", "text/plain", "access_token=a&expires_in=soon", 200, ""},
             {"deep json", "application/json", std::string(100000, '[') + std::string(100000, ']'), 200,
              "oauth2: cannot parse json: "},
         }) {
        SCOPED_TRACE(c.name);
        server ts([&](http::ResponseWriter& w, http::Request&) {
            w.Header().Set("Content-Type", c.contentType);
            if (c.status != 200) w.WriteHeader(c.status);
            io::WriteString(w, c.body);
        });
        auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "code");
        if (std::string(c.wantErr).empty()) {
            EXPECT_FALSE(err) << err;
            EXPECT_NE(tok, nullptr);
        } else {
            ASSERT_TRUE(err);
            EXPECT_NE(S(err.Error()).find(c.wantErr), std::string::npos) << err;
            EXPECT_EQ(tok, nullptr);
        }
    }
}

TEST(GoOAuth2, ExpiresInLimits) {
    auto exchange = [](const std::string& body, const char* ct) {
        server ts([&](http::ResponseWriter& w, http::Request&) { respond(w, ct, body); });
        return newConf(ts.URL()).Exchange(bg(), "code");
    };
    auto now = time::Now();
    auto [big, e1] = exchange(R"({"access_token":"a","expires_in":9223372036854775807})", "application/json");
    ASSERT_FALSE(e1) << e1;
    EXPECT_TRUE(big->Valid());
    EXPECT_EQ(big->ExpiresIn, 2147483647);  // limited to an int32
    EXPECT_TRUE(big->Expiry.After(now.Add(2000000000 * time::Second)));

    auto [neg, e2] = exchange(R"({"access_token":"a","expires_in":-5})", "application/json");
    ASSERT_FALSE(e2) << e2;
    EXPECT_FALSE(neg->Valid());  // expired already
    EXPECT_EQ(neg->ExpiresIn, -5);

    auto [fbig, e3] = exchange("access_token=a&expires_in=99999999999999999999", "text/plain");
    ASSERT_FALSE(e3) << e3;
    EXPECT_TRUE(fbig->Valid());
    auto [fneg, e4] = exchange("access_token=a&expires_in=-99999999999999999999", "text/plain");
    ASSERT_FALSE(e4) << e4;
    EXPECT_FALSE(fneg->Valid());
    auto [fjunk, e5] = exchange("access_token=a&expires_in=soon", "text/plain");
    ASSERT_FALSE(e5) << e5;
    EXPECT_TRUE(fjunk->Expiry.IsZero());
}

TEST(GoOAuth2, TokenResponseBodyLimit) {
    // Only 1 MiB of a body is read: a long error body is cut there, and a
    // token that is longer than that is not a complete JSON.
    server ts([](http::ResponseWriter& w, http::Request& r) {
        if (requestURL(r) == "/token") {
            respond(w, "text/plain", std::string(3 << 20, 'x'), 500);
            return;
        }
        respond(w, "application/json", R"({"access_token":")" + std::string(2 << 20, 'a') + R"("})");
    });
    auto conf = newConf(ts.URL());
    auto [tok, err] = conf.Exchange(bg(), "code");
    ASSERT_TRUE(err);
    auto* re = asRetrieveError(err);
    ASSERT_NE(re, nullptr) << err;
    EXPECT_EQ(re->Body.size(), size_t(1) << 20);

    conf.Endpoint.TokenURL = string(ts.URL() + "/big");
    auto [tok2, err2] = conf.Exchange(bg(), "code");
    ASSERT_TRUE(err2);
    EXPECT_NE(S(err2.Error()).find("oauth2: cannot parse json: "), std::string::npos) << err2;

    // The same for the device authorization endpoint.
    conf.Endpoint.DeviceAuthURL = string(ts.URL() + "/token");
    auto [da, err3] = conf.DeviceAuth(bg());
    ASSERT_TRUE(err3);
    ASSERT_NE(asRetrieveError(err3), nullptr);
    EXPECT_EQ(asRetrieveError(err3)->Body.size(), size_t(1) << 20);
}

TEST(GoOAuth2, ExtraIDToken) {
    server ts([](http::ResponseWriter& w, http::Request&) {
        respond(w, "application/json",
                R"({"access_token":"a","token_type":"Bearer","id_token":"hdr.payload.sig","n":{"x":[1,2]}})");
    });
    auto [tok, err] = newConf(ts.URL()).Exchange(bg(), "code");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(tok->Extra("id_token"), json::Value("hdr.payload.sig"));
    EXPECT_TRUE(tok->Extra("n").IsObject());
    EXPECT_TRUE(tok->Extra("missing").IsNull());
    // The copy of WithExtra is a token of its own.
    auto copy = tok->WithExtra(json::Value(json::Value::object{{"k", "v"}}));
    EXPECT_EQ(S(copy->AccessToken), "a");
    EXPECT_EQ(copy->Extra("k"), json::Value("v"));
    EXPECT_TRUE(copy->Extra("id_token").IsNull());
    EXPECT_EQ(tok->Extra("id_token"), json::Value("hdr.payload.sig"));
}

TEST(GoOAuth2, AutoDetectAuthStyle) {
    // The first request goes with the credentials in the header and, if
    // that fails, in the parameters; what works is remembered for the Config.
    std::vector<std::string> seen;
    server ts([&](http::ResponseWriter& w, http::Request& r) {
        auto auth = S(r.Header.Get("Authorization"));
        auto body = readBody(r);
        seen.push_back(auth.empty() ? "params" : "header");
        if (!auth.empty()) {
            respond(w, "application/json", R"({"error":"invalid_client"})", 401);
            return;
        }
        EXPECT_NE(body.find("client_id=CLIENT_ID"), std::string::npos) << body;
        EXPECT_NE(body.find("client_secret=CLIENT_SECRET"), std::string::npos) << body;
        respond(w, "application/json", R"({"access_token":"a"})");
    });
    auto conf = newConf(ts.URL());
    auto [t1, e1] = conf.Exchange(bg(), "c1");
    ASSERT_FALSE(e1) << e1;
    auto [t2, e2] = conf.Exchange(bg(), "c2");
    ASSERT_FALSE(e2) << e2;
    EXPECT_EQ(seen, (std::vector<std::string>{"header", "params", "params"}));
    // A copy of a Config shares what its original found.
    auto copy = conf;
    auto [t3, e3] = copy.Exchange(bg(), "c3");
    ASSERT_FALSE(e3) << e3;
    EXPECT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen.back(), "params");
}
