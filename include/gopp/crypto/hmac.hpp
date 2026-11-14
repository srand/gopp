// The crypto/hmac package: keyed-hash message authentication codes
// (FIPS 198), over any hash::Hash.
//
//     bool ValidMAC(std::span<const byte> message, std::span<const byte> messageMAC, std::span<const byte> key) {
//         auto mac = hmac::New(sha256::New, key);
//         mac->Write(message);
//         return hmac::Equal(messageMAC, mac->Sum(bytes()));
//     }
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <functional>
#include <span>
#endif

namespace gopp::hmac {

// hmac.New: an HMAC Hash with key over the hashes h makes, such as
// sha256::New. Each call of h must make a new Hash.
ptr<hash::Hash> New(std::function<ptr<hash::Hash>()> h, std::span<const byte> key);

// hmac.Equal: whether two MACs are equal, in time that depends only on
// their lengths.
bool Equal(std::span<const byte> mac1, std::span<const byte> mac2) noexcept;

}  // namespace gopp::hmac
