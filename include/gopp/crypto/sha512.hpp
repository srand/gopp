// The crypto/sha512 package: the SHA-384, SHA-512, SHA-512/224 and SHA-512/256 hash functions (FIPS 180-4), with OpenSSL.
//
//     auto sum = sha512::Sum512("hello world\n");
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <array>
#include <span>
#include <string_view>
#endif

namespace gopp::sha512 {

// The sizes of the checksums in bytes.
inline constexpr isize Size = 64;
inline constexpr isize Size224 = 28;
inline constexpr isize Size256 = 32;
inline constexpr isize Size384 = 48;
// The block size of the hash functions in bytes.
inline constexpr isize BlockSize = 128;

// sha512.New: a Hash computing SHA-512.
ptr<hash::Hash> New();
// sha512.New384: a Hash computing SHA-384.
ptr<hash::Hash> New384();
// sha512.New512_224: a Hash computing SHA-512/224.
ptr<hash::Hash> New512_224();
// sha512.New512_256: a Hash computing SHA-512/256.
ptr<hash::Hash> New512_256();

// The SHA-512 checksum of data.
std::array<byte, 64> Sum512(std::span<const byte> data);
inline std::array<byte, 64> Sum512(std::string_view data) { return Sum512(as_bytes(data)); }
// The SHA-384 checksum of data.
std::array<byte, 48> Sum384(std::span<const byte> data);
inline std::array<byte, 48> Sum384(std::string_view data) { return Sum384(as_bytes(data)); }
// The SHA-512/224 checksum of data.
std::array<byte, 28> Sum512_224(std::span<const byte> data);
inline std::array<byte, 28> Sum512_224(std::string_view data) { return Sum512_224(as_bytes(data)); }
// The SHA-512/256 checksum of data.
std::array<byte, 32> Sum512_256(std::span<const byte> data);
inline std::array<byte, 32> Sum512_256(std::string_view data) { return Sum512_256(as_bytes(data)); }

}  // namespace gopp::sha512
