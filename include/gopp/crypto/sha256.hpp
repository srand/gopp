// The crypto/sha256 package: the SHA-224 and SHA-256 hash functions (FIPS 180-4), with OpenSSL.
//
//     auto sum = sha256::Sum256("hello world\n");
//     fmt::Println("{}", hex::EncodeToString(sum));
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <array>
#include <span>
#include <string_view>
#endif

namespace gopp::sha256 {

// The size of a SHA-256 checksum in bytes, and of a SHA-224 one.
inline constexpr isize Size = 32;
inline constexpr isize Size224 = 28;
// The block size of SHA-256 and SHA-224 in bytes.
inline constexpr isize BlockSize = 64;

// sha256.New: a Hash computing SHA-256.
ptr<hash::Hash> New();
// sha256.New224: a Hash computing SHA-224.
ptr<hash::Hash> New224();

// The SHA-256 checksum of data.
std::array<byte, 32> Sum256(std::span<const byte> data);
inline std::array<byte, 32> Sum256(std::string_view data) { return Sum256(as_bytes(data)); }
// The SHA-224 checksum of data.
std::array<byte, 28> Sum224(std::span<const byte> data);
inline std::array<byte, 28> Sum224(std::string_view data) { return Sum224(as_bytes(data)); }

}  // namespace gopp::sha256
