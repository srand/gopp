// x/jose's jwt, after github.com/go-jose/go-jose/v4/jwt (v4.1.7): JSON Web
// Tokens (RFC 7519) signed as compact JWS.
//
//     auto [tok, err] = jwt::ParseSigned(raw, std::array{jose::RS256});
//     jwt::Claims c;
//     err = tok->Claims(keySet, &c);                       // verifies, then decodes
//     err = c.Validate({.Issuer = "https://issuer", .AnyAudience = {"client"}});
//
//     auto [s, err] = jwt::Signed(signer).Claims(c).Serialize();
//
// Left out of go-jose's: encrypted and nested tokens (JWE), Builder.Token,
// NestedBuilder, and Expected.WithTime (Expected{.Time = t}). Go's nil
// *NumericDate is a null ptr<NumericDate>.
#pragma once

#include <gopp/base.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/jose.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gopp::jose::jwt {

// "go-jose/go-jose/jwt: expected string or array value to unmarshal to Audience"
extern const error ErrUnmarshalAudience;
// "go-jose/go-jose/jwt: expected number value to unmarshal NumericDate"
extern const error ErrUnmarshalNumericDate;
// "go-jose/go-jose/jwt: expected claims to be value convertible into JSON object"
extern const error ErrInvalidClaims;
// The errors of Claims::Validate: "go-jose/go-jose/jwt: validation failed, ..."
extern const error ErrInvalidIssuer, ErrInvalidSubject, ErrInvalidAudience, ErrInvalidID, ErrNotValidYet, ErrExpired,
    ErrIssuedInTheFuture;

// jwt.NumericDate: seconds since the Unix epoch. In JSON a number, of which
// the fraction is dropped; one of 2^62 seconds or more either way is
// ErrUnmarshalNumericDate.
struct NumericDate : object {
    int64 seconds = 0;

    NumericDate() = default;
    explicit NumericDate(int64 s) noexcept : seconds(s) {}

    // n.Time(): the time of n.
    time::Time Time() const;

    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> b);
};

// jwt.NewNumericDate: the date of t, null for the zero time.
ptr<NumericDate> NewNumericDate(time::Time t);

// jwt.Audience: the "aud" claim, in JSON a string or a list of strings
// (one is written as a string).
struct Audience : slice<string> {
    using slice<string>::slice;
    Audience() = default;
    Audience(slice<string> s) noexcept : slice<string>(std::move(s)) {}

    // s.Contains(v): whether v is one of the audiences.
    bool Contains(std::string_view v) const;

    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> b);
};

// jwt.Expected: the values that Claims::Validate checks; the empty ones
// aren't checked.
struct Expected {
    // Initialized, so that designated initializers can leave members out.
    string Issuer{};
    string Subject{};
    string ID{};
    // Any of these must be in the audience.
    jwt::Audience AnyAudience{};
    // The time of validation; time::Now() if zero.
    time::Time Time{};
};

// jwt.DefaultLeeway: the leeway of Claims::Validate.
inline constexpr time::Duration DefaultLeeway = time::Minute;

// jwt.Claims: the registered claims of RFC 7519.
struct Claims {
    [[= json::tag("iss,omitempty")]] string Issuer;
    [[= json::tag("sub,omitempty")]] string Subject;
    [[= json::tag("aud,omitempty")]] jwt::Audience Audience;
    [[= json::tag("exp,omitempty")]] ptr<NumericDate> Expiry;
    [[= json::tag("nbf,omitempty")]] ptr<NumericDate> NotBefore;
    [[= json::tag("iat,omitempty")]] ptr<NumericDate> IssuedAt;
    [[= json::tag("jti,omitempty")]] string ID;

    // c.Validate(e): ValidateWithLeeway(e, DefaultLeeway).
    error Validate(const Expected& e) const;
    // c.ValidateWithLeeway(e, leeway): checks the claims against e, and the
    // times of the claims that are there against e.Time, allowing for
    // leeway. It doesn't check the signature: Claims of a JSONWebToken
    // does.
    error ValidateWithLeeway(const Expected& e, time::Duration leeway) const;
};

// jwt.JSONWebToken: a parsed token.
class JSONWebToken : public object {
public:
    // The headers of its signatures: one.
    slice<jose::Header> Headers;

    // t.Claims(key, out...): verifies the token with key (as
    // JSONWebSignature::Verify does) and decodes its claims into each out.
    template <typename... T>
    error Claims(const object& key, T*... out) const {
        auto [b, err] = payload(key);
        if (err) return err;
        return unmarshalAll(b, out...);
    }

    // t.UnsafeClaimsWithoutVerification(out...): decodes the claims without
    // verifying them. They cannot be trusted.
    template <typename... T>
    error UnsafeClaimsWithoutVerification(T*... out) const {
        if (!sig_) return errors::New("go-jose/go-jose: Cannot get unverified claims");
        return unmarshalAll(sig_->UnsafePayloadWithoutVerification(), out...);
    }

    ptr<jose::JSONWebSignature> sig_;  // internal

private:
    std::pair<slice<byte>, error> payload(const object& key) const;

    template <typename... T>
    static error unmarshalAll([[maybe_unused]] std::span<const byte> b, T*... out) {
        error err;
        (void)((err = json::Unmarshal(b, out), !err) && ...);
        return err;
    }
};

// jwt.ParseSigned: parses a token in compact serialization (as
// jose::ParseSignedCompact, so its algorithm must be one of allowedAlgs).
std::pair<ptr<JSONWebToken>, error> ParseSigned(std::string_view s, std::span<const std::string_view> allowedAlgs);

namespace detail {

template <typename T>
inline constexpr bool isPtr = false;
template <typename T>
inline constexpr bool isPtr<ptr<T>> = true;

// Go's map[string]interface{}.
template <typename T>
concept anyMap = requires {
    typename T::key_type;
    typename T::mapped_type;
} && std::is_same_v<typename T::mapped_type, json::Value> && std::is_convertible_v<typename T::key_type, std::string_view>;

// normalize: the members of the JSON object of c, as they are encoded.
template <typename T>
std::pair<std::map<string, json::RawMessage>, error> normalize(const T& c) {
    auto [raw, err] = json::Marshal(c);
    if (err) return {{}, err};
    std::map<string, json::RawMessage> m;
    if (error err2 = json::Unmarshal(raw, &m)) return {{}, err2};
    return {std::move(m), nullptr};
}

// The claims of c: a struct, a map of json::Value (Go's
// map[string]interface{}) or json::Value object, or a pointer to one.
// Others are ErrInvalidClaims.
template <typename T>
std::pair<std::map<string, json::RawMessage>, error> claimsOf(const T& c) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_pointer_v<U> || isPtr<U>) {
        if constexpr (std::is_class_v<std::remove_cvref_t<decltype(*c)>>) {
            if (!c) return {{}, ErrInvalidClaims};
            return claimsOf(*c);
        } else {
            return {{}, ErrInvalidClaims};
        }
    } else if constexpr (std::is_same_v<U, json::Value>) {
        if (!c.IsObject()) return {{}, ErrInvalidClaims};
        return normalize(c);
    } else if constexpr (anyMap<U>) {
        return normalize(c);
    } else if constexpr (std::is_class_v<U> && !std::ranges::range<U>) {
        return normalize(c);
    } else {
        return {{}, ErrInvalidClaims};
    }
}

}  // namespace detail

// jwt.Builder, of signed tokens: Signed(signer).Claims(c)...Serialize().
class Builder {
public:
    // b.Claims(c): a builder of these claims as well, which replace those
    // of the same names. c is a struct (as encoded by json::Marshal), a
    // std::map<string, json::Value>, a json::Value object, or a pointer
    // to one; others are ErrInvalidClaims, when serializing.
    template <typename T>
    Builder Claims(const T& c) const {
        if (err_) return *this;
        Builder b;
        b.sig_ = sig_;
        auto [m, err] = detail::claimsOf(c);
        if (err) {
            b.err_ = err;
            return b;
        }
        b.payload_ = payload_ ? *payload_ : std::map<string, json::RawMessage>{};
        for (auto& [k, v] : m) b.payload_->insert_or_assign(k, std::move(v));
        return b;
    }

    // b.Serialize(): the signed token, in compact serialization.
    std::pair<string, error> Serialize() const;

    ptr<jose::Signer> sig_;                                     // internal
    std::optional<std::map<string, json::RawMessage>> payload_;  // internal: Go's nil map is none
    error err_;                                                 // internal
};

// jwt.Signed: a Builder of tokens signed by sig.
Builder Signed(ptr<jose::Signer> sig);

}  // namespace gopp::jose::jwt
