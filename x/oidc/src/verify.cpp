// ID token verification, ported from coreos/go-oidc v3.21.0's verify.go
// (without distributed claims and logout tokens), and the JSON types of an
// ID token's claims from oidc.go.
#include "oidc_internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gopp::oidc {

namespace {

constexpr std::string_view issuerGoogleAccounts = "https://accounts.google.com";
constexpr std::string_view issuerGoogleAccountsNoScheme = "accounts.google.com";

// audience: "aud", a string or a list of them.
struct audience {
    slice<string> v;
    error UnmarshalJSON(std::span<const byte> b) {
        string s;
        if (!json::Unmarshal(b, &s)) {
            v = {s};
            return nullptr;
        }
        slice<string> auds;
        if (error err = json::Unmarshal(b, &auds)) return err;
        v = auds;
        return nullptr;
    }
};

// The bound of a jsonTime: 2^62 seconds either way.
constexpr int64 maxUnix = int64(1) << 62;
const error errTimeRange = errors::New("oidc: time out of range");

// jsonTime: a time in seconds since the epoch, a JSON number (or a string
// of one, as Go's json.Number takes it). An integer is read as one; other
// numbers are truncated. 2^62 seconds or more either way is an error.
struct jsonTime {
    time::Time t;
    error UnmarshalJSON(std::span<const byte> b) {
        std::string_view s = asStringView(b);
        string text;
        if (s.starts_with('"')) {
            if (error err = json::Unmarshal(b, &text)) return err;
        } else {
            text = string(s);
        }
        // json.Number: the text of a valid JSON number.
        json::Value n;
        if (json::Unmarshal(text.view(), &n) || !n.IsNumber() || strings::TrimSpace(text.view()) != text.view()) {
            return fmt::Errorf("json: invalid number literal {}", strconv::Quote(text.view()));
        }
        int64 unix;
        if (auto [i, err] = strconv::ParseInt(text.view(), 10, 64); !err) {
            unix = i;
        } else {
            auto [f, err2] = strconv::ParseFloat(text.view(), 64);
            if (err2) return err2;
            if (!(f > -maxUnix && f < maxUnix)) return errTimeRange;
            unix = static_cast<int64>(f);
        }
        // Go's time.Unix wraps around for these, and int64(f) of a larger
        // float is undefined in C++: rejected, as go-jose's NumericDate is.
        if (unix <= -maxUnix || unix >= maxUnix) return errTimeRange;
        t = time::Unix(unix, 0);
        return nullptr;
    }
};

// idToken: the claims of an ID token that Verify reads.
struct idToken {
    [[= json::tag("iss")]] string Issuer;
    [[= json::tag("sub")]] string Subject;
    [[= json::tag("aud")]] audience Audience;
    [[= json::tag("exp")]] jsonTime Expiry;
    [[= json::tag("iat")]] jsonTime IssuedAt;
    [[= json::tag("nbf")]] std::optional<jsonTime> NotBefore;
    [[= json::tag("nonce")]] string Nonce;
    [[= json::tag("at_hash")]] string AtHash;
};

// Go's %q of a []string: ["a" "b"].
string quoteList(const slice<string>& l) {
    std::string out = "[";
    for (size_t i = 0; i < l.size(); i++) {
        if (i > 0) out += ' ';
        out += strconv::Quote(l[i].view()).view();
    }
    out += ']';
    return string(out);
}

}  // namespace

ptr<IDTokenVerifier> NewVerifier(std::string_view issuerURL, ptr<KeySet> keySet, const Config& config) {
    auto v = make_ptr<IDTokenVerifier>();
    v->keySet_ = std::move(keySet);
    v->config_ = config;
    v->issuer_ = string(issuerURL);
    return v;
}

ptr<IDTokenVerifier> Provider::VerifierContext(ptr<context::Context> ctx, const Config& config) const {
    return newVerifier(NewRemoteKeySet(std::move(ctx), jwksURL_.view()), config);
}

ptr<IDTokenVerifier> Provider::Verifier(const Config& config) const { return newVerifier(remoteKeySet(), config); }

ptr<IDTokenVerifier> Provider::newVerifier(ptr<KeySet> keySet, const Config& config) const {
    if (config.SupportedSigningAlgs.empty() && !algorithms_.empty()) {
        Config cp = config;
        cp.SupportedSigningAlgs = algorithms_;
        return NewVerifier(issuer_.view(), std::move(keySet), cp);
    }
    return NewVerifier(issuer_.view(), std::move(keySet), config);
}

std::pair<ptr<IDToken>, error> IDTokenVerifier::Verify(ptr<context::Context> ctx, std::string_view rawIDToken) const {
    auto [payload, sigAlgorithm, err] = verifyJWT(std::move(ctx), rawIDToken);
    if (err) return {nullptr, err};
    // Case-sensitive, as go-jose's claims (go-oidc uses encoding/json).
    idToken token;
    if (error e = json::UnmarshalCaseSensitive(payload, &token)) {
        return {nullptr, fmt::Errorf("oidc: failed to unmarshal claims: {}", e)};
    }

    auto t = make_ptr<IDToken>();
    t->Issuer = token.Issuer;
    t->Subject = token.Subject;
    t->Audience = token.Audience.v;
    t->Expiry = token.Expiry.t;
    t->IssuedAt = token.IssuedAt.t;
    t->Nonce = token.Nonce;
    t->AccessTokenHash = token.AtHash;
    t->claims_ = payload;
    t->hasClaims_ = true;
    t->sigAlgorithm_ = sigAlgorithm;

    // Check issuer.
    if (!config_.SkipIssuerCheck && t->Issuer != issuer_) {
        // Google sometimes returns "accounts.google.com" as the issuer claim
        // instead of the required "https://accounts.google.com". Detect this
        // case and allow it only for Google.
        //
        // We will not add hooks to let other providers go off spec like this.
        if (!(issuer_.view() == issuerGoogleAccounts && t->Issuer.view() == issuerGoogleAccountsNoScheme)) {
            return {nullptr, fmt::Errorf("oidc: id token issued by a different provider, expected {} got {}",
                                         strconv::Quote(issuer_.view()), strconv::Quote(t->Issuer.view()))};
        }
    }

    // If a client ID has been provided, make sure it's part of the audience.
    // SkipClientIDCheck must be true if ClientID is empty.
    //
    // This check DOES NOT ensure that the ClientID is the party to which the
    // ID Token was issued (i.e. Authorized party).
    if (!config_.SkipClientIDCheck) {
        if (config_.ClientID.empty()) {
            return {nullptr, errors::New(
                                 "oidc: invalid configuration, clientID must be provided or SkipClientIDCheck must be set")};
        }
        if (std::find(t->Audience.begin(), t->Audience.end(), config_.ClientID) == t->Audience.end()) {
            return {nullptr, fmt::Errorf("oidc: expected audience {} got {}", strconv::Quote(config_.ClientID.view()),
                                         quoteList(t->Audience))};
        }
    }

    // If a SkipExpiryCheck is false, make sure token is not expired.
    if (!config_.SkipExpiryCheck) {
        time::Time nowTime = config_.Now ? config_.Now() : time::Now();
        if (t->Expiry.Before(nowTime)) {
            auto e = make_ptr<TokenExpiredError>();
            e->Expiry = t->Expiry;
            return {nullptr, e};
        }
        // If nbf claim is provided in token, ensure that it is indeed in the
        // past. 5 minutes of leeway, as other OpenID Connect providers allow
        // for clock skew.
        if (token.NotBefore) {
            time::Time nbfTime = token.NotBefore->t;
            if (nowTime.Add(5 * time::Minute).Before(nbfTime)) {
                return {nullptr,
                        fmt::Errorf("oidc: current time {} before the nbf (not before) time: {}", nowTime, nbfTime)};
            }
        }
    }
    return {t, nullptr};
}

std::tuple<slice<byte>, string, error> IDTokenVerifier::verifyJWT(ptr<context::Context> ctx,
                                                                  std::string_view rawIDToken) const {
    std::vector<std::string_view> supportedSigAlgs;
    for (const auto& alg : config_.SupportedSigningAlgs) supportedSigAlgs.push_back(alg.view());
    if (supportedSigAlgs.empty()) {
        // If no algorithms were specified by both the config and discovery,
        // default to the one mandatory algorithm "RS256".
        supportedSigAlgs.push_back(RS256);
    }
    if (config_.InsecureSkipSignatureCheck) {
        // "none" is a required value to even parse a JWT with the "none"
        // algorithm.
        supportedSigAlgs.push_back("none");
    }

    // Parse and verify the signature first. This at least forces the user
    // to have a valid, signed ID token before we do any other processing.
    auto [jws, err] = jose::ParseSigned(rawIDToken, supportedSigAlgs);
    if (err) return {slice<byte>(), "", fmt::Errorf("oidc: malformed jwt: {}", err)};
    if (jws->Signatures.empty()) return {slice<byte>(), "", errors::New("oidc: id token not signed")};
    if (jws->Signatures.size() > 1) {
        return {slice<byte>(), "", errors::New("oidc: multiple signatures on id token not supported")};
    }
    string alg = jws->Signatures[0].Header.Algorithm;

    if (config_.InsecureSkipSignatureCheck) {
        // Yolo mode.
        return {jws->UnsafePayloadWithoutVerification(), alg, nullptr};
    }
    if (!keySet_) return {slice<byte>(), "", errors::New("failed to verify signature: oidc: no key set")};
    // The JWT is attached here for the happy path to avoid the verifier from
    // having to parse the JWT twice.
    auto vctx = context::WithValue(ctx ? std::move(ctx) : context::Background(), internal::parsedJWTKey, jws);
    auto [payload, err2] = keySet_->VerifySignature(vctx, rawIDToken);
    if (err2) return {slice<byte>(), "", fmt::Errorf("failed to verify signature: {}", err2)};
    return {payload, alg, nullptr};
}

}  // namespace gopp::oidc
