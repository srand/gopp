// The device flow (RFC 8628), ported from golang.org/x/oauth2 v0.36.0's
// deviceauth.go.
#include "oauth2_internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/mime.hpp>
#include <gopp/strings.hpp>

#include <functional>

namespace gopp::oauth2 {

namespace {

// The JSON of a DeviceAuthResponse, as written. (Go's has an Alias of the
// struct under an outer expires_in, which the Expiry field's own
// expires_in is shadowed by.)
struct deviceAuthWrite {
    [[= json::tag("expires_in,omitempty")]] int64 ExpiresIn = 0;
    [[= json::tag("device_code")]] string DeviceCode;
    [[= json::tag("user_code")]] string UserCode;
    [[= json::tag("verification_uri")]] string VerificationURI;
    [[= json::tag("verification_uri_complete,omitempty")]] string VerificationURIComplete;
    [[= json::tag("interval,omitempty")]] int64 Interval = 0;
};

// And as read: with the verification_url that Google sends.
struct deviceAuthRead {
    [[= json::tag("expires_in")]] int64 ExpiresIn = 0;
    [[= json::tag("verification_url")]] string VerificationURL;
    [[= json::tag("device_code")]] string DeviceCode;
    [[= json::tag("user_code")]] string UserCode;
    [[= json::tag("verification_uri")]] string VerificationURI;
    [[= json::tag("verification_uri_complete")]] string VerificationURIComplete;
    [[= json::tag("interval")]] int64 Interval = 0;
};

// Runs f when it goes out of scope (Go's defer).
struct deferred {
    std::function<void()> f;
    ~deferred() { f(); }
};

}  // namespace

std::pair<slice<byte>, error> DeviceAuthResponse::MarshalJSON() const {
    deviceAuthWrite w;
    if (!Expiry.IsZero()) w.ExpiresIn = static_cast<int64>(time::Until(Expiry).Seconds());
    w.DeviceCode = DeviceCode;
    w.UserCode = UserCode;
    w.VerificationURI = VerificationURI;
    w.VerificationURIComplete = VerificationURIComplete;
    w.Interval = Interval;
    return json::Marshal(w);
}

error DeviceAuthResponse::UnmarshalJSON(std::span<const byte> data) {
    deviceAuthRead aux;
    if (error err = json::Unmarshal(data, &aux)) return err;
    DeviceCode = aux.DeviceCode;
    UserCode = aux.UserCode;
    VerificationURI = aux.VerificationURI;
    VerificationURIComplete = aux.VerificationURIComplete;
    Interval = aux.Interval;
    if (aux.ExpiresIn != 0) {
        // Seconds of a Duration, as Go converts them: a hostile expires_in
        // beyond what a Duration holds is as far away as one can be.
        constexpr int64 maxSeconds = INT64_MAX / 1000000000 / 2;
        int64 s = aux.ExpiresIn > maxSeconds ? maxSeconds : aux.ExpiresIn < -maxSeconds ? -maxSeconds : aux.ExpiresIn;
        Expiry = time::Now().UTC().Add(s * time::Second);
    }
    if (VerificationURI.empty()) VerificationURI = aux.VerificationURL;
    return nullptr;
}

namespace {

std::pair<ptr<DeviceAuthResponse>, error> retrieveDeviceAuth(const ptr<context::Context>& ctx, const Config& c,
                                                             const url::Values& v) {
    if (c.Endpoint.DeviceAuthURL.empty()) return {nullptr, errors::New("endpoint missing DeviceAuthURL")};

    auto [req, err] = http::NewRequest("POST", c.Endpoint.DeviceAuthURL.view(), strings::NewReader(v.Encode()));
    if (err) return {nullptr, err};
    req->Header.Set("Content-Type", "application/x-www-form-urlencoded");
    req->Header.Set("Accept", "application/json");

    auto t = time::Now();
    auto [r, derr] = internal::ContextClient(ctx).Do(ctx ? req->WithContext(ctx) : req);
    if (derr) return {nullptr, derr};

    slice<byte> body;
    if (r->Body) {
        auto [b, rerr] = io::ReadAll(*io::LimitReader(r->Body, 1 << 20));
        r->Body->Close();
        if (rerr) return {nullptr, errors::New("oauth2: cannot auth device: " + std::string(rerr.Error().view()))};
        body = std::move(b);
    }
    if (int code = r->StatusCode; code < 200 || code > 299) {
        auto retrieveError = make_ptr<RetrieveError>();
        retrieveError->Response = r;
        retrieveError->Body = body;

        auto [content, params, merr] = mime::ParseMediaType(r->Header.Get("Content-Type").view());
        if (content.view() == "application/x-www-form-urlencoded" || content.view() == "text/plain") {
            auto [vals, perr] = url::ParseQuery(asStringView(body));
            if (perr) return {nullptr, retrieveError};
            retrieveError->ErrorCode = vals.Get("error");
            retrieveError->ErrorDescription = vals.Get("error_description");
            retrieveError->ErrorURI = vals.Get("error_uri");
        } else {
            struct errorJSON {
                [[= json::tag("error")]] string ErrorCode;
                [[= json::tag("error_description")]] string ErrorDescription;
                [[= json::tag("error_uri")]] string ErrorURI;
            } tj;
            if (json::Unmarshal(body, &tj)) return {nullptr, retrieveError};
            retrieveError->ErrorCode = tj.ErrorCode;
            retrieveError->ErrorDescription = tj.ErrorDescription;
            retrieveError->ErrorURI = tj.ErrorURI;
        }
        return {nullptr, retrieveError};
    }

    auto da = make_ptr<DeviceAuthResponse>();
    if (error uerr = da->UnmarshalJSON(body)) return {nullptr, errors::New("unmarshal " + std::string(uerr.Error().view()))};

    if (!da->Expiry.IsZero()) da->Expiry = da->Expiry.Add(-time::Since(t));
    return {da, nullptr};
}

}  // namespace

std::pair<ptr<DeviceAuthResponse>, error> Config::DeviceAuth(ptr<context::Context> ctx,
                                                             std::span<const AuthCodeOption> opts) const {
    url::Values v;
    v.Set("client_id", ClientID);
    if (!Scopes.empty()) v.Set("scope", strings::Join(std::span<const string>(Scopes.data(), Scopes.size()), " "));
    for (auto& opt : opts)
        for (auto& [k, val] : opt.params) v.Set(k, val);
    return retrieveDeviceAuth(ctx, *this, v);
}

std::pair<ptr<Token>, error> Config::DeviceAccessToken(ptr<context::Context> ctx, const DeviceAuthResponse& da,
                                                       std::span<const AuthCodeOption> opts) const {
    if (!ctx) ctx = context::Background();
    // The poll ends when the device code does.
    context::CancelFunc cancel;
    if (!da.Expiry.IsZero()) std::tie(ctx, cancel) = context::WithDeadline(ctx, da.Expiry);
    deferred cancelAtEnd{[&] {
        if (cancel) cancel();
    }};

    url::Values v;
    v.Set("client_id", ClientID);
    v.Set("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    v.Set("device_code", da.DeviceCode);
    if (!Scopes.empty()) v.Set("scope", strings::Join(std::span<const string>(Scopes.data(), Scopes.size()), " "));
    for (auto& opt : opts)
        for (auto& [k, val] : opt.params) v.Set(k, val);

    // "If no value is provided, clients MUST use 5 as the default."
    int64 interval = da.Interval;
    if (interval == 0) interval = 5;

    auto ticker = time::NewTicker(interval * time::Second);
    deferred stopTicker{[&] { ticker->Stop(); }};
    for (;;) {
        bool done = false, tick = false;
        select(recv(ctx->Done(), [&] { done = true; }), recv(ticker->C, [&] { tick = true; }));
        if (done) return {nullptr, ctx->Err()};
        if (!tick) continue;

        auto [tok, err] = internal::retrieveToken(ctx, *this, v);
        if (!err) return {tok, nullptr};

        auto* e = dynamic_cast<const RetrieveError*>(err.get());
        if (!e) return {nullptr, err};
        if (e->ErrorCode.view() == internal::errSlowDown) {
            // "the interval MUST be increased by 5 seconds for this and all
            // subsequent requests"
            interval += 5;
            ticker->Reset(interval * time::Second);
        } else if (e->ErrorCode.view() == internal::errAuthorizationPending) {
            // Do nothing.
        } else {
            // access_denied, expired_token, and the others end the flow.
            return {nullptr, err};
        }
    }
}

}  // namespace gopp::oauth2
