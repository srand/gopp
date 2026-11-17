// PKCE (RFC 7636), ported from golang.org/x/oauth2 v0.36.0's pkce.go.
#include "oauth2_internal.hpp"

#include <gopp/crypto/rand.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/encoding/base64.hpp>

#include <array>

namespace gopp::oauth2 {

namespace {

constexpr std::string_view codeChallengeKey = "code_challenge";
constexpr std::string_view codeChallengeMethodKey = "code_challenge_method";
constexpr std::string_view codeVerifierKey = "code_verifier";

}  // namespace

string GenerateVerifier() {
    // "RECOMMENDED that the output of a suitable random number generator be
    // used to create a 32-octet sequence. The octet sequence is then
    // base64url-encoded to produce a 43-octet URL-safe string to use as the
    // code verifier." (RFC 7636, section 4.1)
    std::array<byte, 32> data;
    rand::Read(data);
    return base64::RawURLEncoding.EncodeToString(data);
}

AuthCodeOption VerifierOption(std::string_view verifier) { return SetAuthURLParam(codeVerifierKey, verifier); }

string S256ChallengeFromVerifier(std::string_view verifier) {
    auto sha = sha256::Sum256(verifier);
    return base64::RawURLEncoding.EncodeToString(sha);
}

AuthCodeOption S256ChallengeOption(std::string_view verifier) {
    return {{{string(codeChallengeMethodKey), "S256"}, {string(codeChallengeKey), S256ChallengeFromVerifier(verifier)}}};
}

}  // namespace gopp::oauth2
