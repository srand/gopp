// The crypto/sha1 package: the SHA-1 hash function (RFC 3174). SHA-1 is broken: use it for
// compatibility only, not for security, with OpenSSL.
//
//     auto sum = sha1::Sum("hello world\n");
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <array>
#include <span>
#include <string_view>
#endif

namespace gopp::sha1 {

// The size of a SHA-1 checksum in bytes.
inline constexpr isize Size = 20;
// The block size of SHA-1 in bytes.
inline constexpr isize BlockSize = 64;

// sha1.New: a Hash computing SHA-1.
ptr<hash::Hash> New();

// The SHA-1 checksum of data.
std::array<byte, 20> Sum(std::span<const byte> data);
inline std::array<byte, 20> Sum(std::string_view data) { return Sum(as_bytes(data)); }

}  // namespace gopp::sha1
