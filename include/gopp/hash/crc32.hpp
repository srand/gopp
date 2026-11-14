// The hash/crc32 package: the 32-bit cyclic redundancy check, ported from
// Go 1.27.3. IEEE checksums are computed by zlib's crc32, the others with
// Go's slicing-by-8 tables.
//
//     uint32 sum = crc32::ChecksumIEEE(data);          // as gzip and zip have it
//     auto tab = crc32::MakeTable(crc32::Castagnoli);  // as iSCSI and ext4 have it
//     auto h = crc32::New(tab);
//     io::Copy(*h, *file);
//     h->Sum32();
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/hash.hpp>
#include <gopp/ptr.hpp>

#include <array>
#include <span>
#endif

namespace gopp::crc32 {

// crc32.Size: the size of a checksum in bytes.
inline constexpr isize Size = 4;

// Predefined polynomials, reversed.
inline constexpr uint32 IEEE = 0xedb88320;        // by far the most common
inline constexpr uint32 Castagnoli = 0x82f63b78;  // used by iSCSI, ext4, ...
inline constexpr uint32 Koopman = 0xeb31d82e;

// crc32.Table: the table of a polynomial, for efficient processing.
using Table = std::array<uint32, 256>;

namespace detail {
consteval Table makeTable(uint32 poly) {
    Table t{};
    for (uint32 i = 0; i < 256; i++) {
        uint32 crc = i;
        for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ poly : crc >> 1;
        t[i] = crc;
    }
    return t;
}
inline constexpr Table ieeeTable = makeTable(IEEE);
}  // namespace detail

// crc32.IEEETable: the table of the IEEE polynomial.
inline constexpr const Table* IEEETable = &detail::ieeeTable;

// crc32.MakeTable: the table of the polynomial poly. Tables are made once
// per polynomial and kept.
const Table* MakeTable(uint32 poly);

// crc32.Update: crc updated with p.
uint32 Update(uint32 crc, const Table* tab, std::span<const byte> p);
// crc32.Checksum: the checksum of data with the polynomial of tab.
uint32 Checksum(std::span<const byte> data, const Table* tab);
// crc32.ChecksumIEEE: the IEEE checksum of data.
uint32 ChecksumIEEE(std::span<const byte> data);

// crc32.New: a Hash32 with the polynomial of tab. Its Sum appends the
// checksum big-endian.
ptr<hash::Hash32> New(const Table* tab);
// crc32.NewIEEE
ptr<hash::Hash32> NewIEEE();

}  // namespace gopp::crc32
