// The crypto/md5 package: the MD5 hash function (RFC 1321). MD5 is broken: use it for
// compatibility only, not for security, with OpenSSL.
//
//     auto sum = md5::Sum("hello world\n");
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <array>
#include <span>
#include <string_view>
#endif

namespace gopp::md5 {

// The size of an MD5 checksum in bytes.
inline constexpr isize Size = 16;
// The block size of MD5 in bytes.
inline constexpr isize BlockSize = 64;

// md5.New: a Hash computing MD5.
ptr<hash::Hash> New();

// The MD5 checksum of data.
std::array<byte, 16> Sum(std::span<const byte> data);
inline std::array<byte, 16> Sum(std::string_view data) { return Sum(as_bytes(data)); }

}  // namespace gopp::md5
