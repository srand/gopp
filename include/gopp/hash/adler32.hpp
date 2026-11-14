// The hash/adler32 package: the Adler-32 checksum of RFC 1950 (zlib's),
// ported from Go 1.27.3.
//
//     uint32 sum = adler32::Checksum(data);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <span>
#endif

namespace gopp::adler32 {

// adler32.Size: the size of a checksum in bytes.
inline constexpr isize Size = 4;

// adler32.New: a Hash32 of Adler-32. Its Sum appends the checksum
// big-endian.
ptr<hash::Hash32> New();
// adler32.Checksum: the Adler-32 checksum of data.
uint32 Checksum(std::span<const byte> data);

}  // namespace gopp::adler32
