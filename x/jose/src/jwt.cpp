// JSON Web Tokens, ported from go-jose v4.1.7's jwt/claims.go,
// jwt/validation.go, jwt/jwt.go, jwt/builder.go and jwt/errors.go.
#include <gopp/jose/jwt.hpp>
#include <gopp/strings.hpp>

#include <cmath>
#include <string>

namespace gopp::jose::jwt {

const error ErrUnmarshalAudience =
    errors::New("go-jose/go-jose/jwt: expected string or array value to unmarshal to Audience");
const error ErrUnmarshalNumericDate = errors::New("go-jose/go-jose/jwt: expected number value to unmarshal NumericDate");
const error ErrInvalidClaims =
    errors::New("go-jose/go-jose/jwt: expected claims to be value convertible into JSON object");
const error ErrInvalidIssuer = errors::New("go-jose/go-jose/jwt: validation failed, invalid issuer claim (iss)");
const error ErrInvalidSubject = errors::New("go-jose/go-jose/jwt: validation failed, invalid subject claim (sub)");
const error ErrInvalidAudience = errors::New("go-jose/go-jose/jwt: validation failed, invalid audience claim (aud)");
const error ErrInvalidID = errors::New("go-jose/go-jose/jwt: validation failed, invalid ID claim (jti)");
const error ErrNotValidYet = errors::New("go-jose/go-jose/jwt: validation failed, token not valid yet (nbf)");
const error ErrExpired = errors::New("go-jose/go-jose/jwt: validation failed, token is expired (exp)");
const error ErrIssuedInTheFuture =
    errors::New("go-jose/go-jose/jwt: validation failed, token issued in the future (iat)");

// ---------------------------------------------------------------------------
// NumericDate

namespace {

// maxNumericDate: 2^62 seconds, beyond which time::Unix would overflow.
constexpr double maxNumericDate = 4611686018427387904.0;

}  // namespace

ptr<NumericDate> NewNumericDate(time::Time t) {
    if (t.IsZero()) return nullptr;
    return make_ptr<NumericDate>(t.Unix());
}

time::Time NumericDate::Time() const { return time::Unix(seconds, 0); }

std::pair<slice<byte>, error> NumericDate::MarshalJSON() const { return {toBytes(std::to_string(seconds)), nullptr}; }

error NumericDate::UnmarshalJSON(std::span<const byte> b) {
    auto [f, err] = strconv::ParseFloat(asStringView(b), 64);
    if (err) return ErrUnmarshalNumericDate;
    // Go's ParseFloat takes "NaN", which Go converts to some integer; its
    // conversion is undefined in C++, so it is out of range here.
    if (std::isnan(f) || f >= maxNumericDate || f <= -maxNumericDate) return ErrUnmarshalNumericDate;
    seconds = static_cast<int64>(f);  // toward zero, as Go's conversion
    return nullptr;
}

// ---------------------------------------------------------------------------
// Audience

bool Audience::Contains(std::string_view v) const {
    for (const auto& a : *this) {
        if (a == v) return true;
    }
    return false;
}

std::pair<slice<byte>, error> Audience::MarshalJSON() const {
    if (size() == 1) return json::Marshal((*this)[0]);
    return json::Marshal(static_cast<const slice<string>&>(*this));
}

error Audience::UnmarshalJSON(std::span<const byte> b) {
    json::Value v;
    if (error err = json::UnmarshalCaseSensitive(b, &v)) return err;
    if (v.IsString()) {
        *this = Audience{v.AsString()};
        return nullptr;
    }
    if (!v.IsArray()) return ErrUnmarshalAudience;
    auto a = slice<string>::make(0, v.size());
    for (const auto& e : v.AsArray()) {
        if (!e.IsString()) return ErrUnmarshalAudience;
        a = append(a, e.AsString());
    }
    *this = Audience(std::move(a));
    return nullptr;
}

// ---------------------------------------------------------------------------
// Validation

error Claims::Validate(const Expected& e) const { return ValidateWithLeeway(e, DefaultLeeway); }

error Claims::ValidateWithLeeway(const Expected& e, time::Duration leeway) const {
    if (!e.Issuer.empty() && e.Issuer != Issuer) return ErrInvalidIssuer;
    if (!e.Subject.empty() && e.Subject != Subject) return ErrInvalidSubject;
    if (!e.ID.empty() && e.ID != ID) return ErrInvalidID;

    if (!e.AnyAudience.empty()) {
        bool intersection = false;
        for (const auto& v : e.AnyAudience) {
            if (Audience.Contains(v)) {
                intersection = true;
                break;
            }
        }
        if (!intersection) return ErrInvalidAudience;
    }

    time::Time validationTime = e.Time;
    if (validationTime.IsZero()) validationTime = time::Now();

    if (NotBefore && validationTime.Add(leeway).Before(NotBefore->Time())) return ErrNotValidYet;
    if (Expiry && validationTime.Add(-leeway).After(Expiry->Time())) return ErrExpired;
    // IssuedAt is optional but cannot be in the future. This is not
    // required by the RFC, but something is misconfigured if this happens.
    if (IssuedAt && validationTime.Add(leeway).Before(IssuedAt->Time())) return ErrIssuedInTheFuture;
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSONWebToken

std::pair<slice<byte>, error> JSONWebToken::payload(const object& key) const {
    // Go's token without a payload function panics.
    if (!sig_) return {slice<byte>(), errors::New("go-jose/go-jose/jwt: token was not parsed")};
    return sig_->Verify(key);
}

std::pair<ptr<JSONWebToken>, error> ParseSigned(std::string_view s, std::span<const std::string_view> allowedAlgs) {
    auto [sig, err] = jose::ParseSignedCompact(s, allowedAlgs);
    if (err) return {nullptr, err};
    auto tok = make_ptr<JSONWebToken>();
    tok->Headers = slice<jose::Header>::make(0, sig->Signatures.size());
    for (const auto& signature : sig->Signatures) tok->Headers = append(tok->Headers, signature.Header);
    tok->sig_ = sig;
    return {tok, nullptr};
}

// ---------------------------------------------------------------------------
// Builder

Builder Signed(ptr<jose::Signer> sig) {
    Builder b;
    b.sig_ = std::move(sig);
    return b;
}

std::pair<string, error> Builder::Serialize() const {
    if (err_) return {"", err_};
    // Go's builder of a nil signer panics.
    if (!sig_) return {"", errors::New("go-jose/go-jose/jwt: no signer")};
    slice<byte> p = toBytes("null");  // Go's nil map
    if (payload_) {
        auto [b, err] = json::Marshal(*payload_);
        if (err) return {"", err};
        p = b;
    }
    auto [obj, err] = sig_->Sign(p);
    if (err) return {"", err};
    return obj->CompactSerialize();
}

}  // namespace gopp::jose::jwt
