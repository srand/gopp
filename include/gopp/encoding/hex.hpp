// The encoding/hex package: hexadecimal encoding and decoding, ported from
// Go 1.27.3.
//
//     string s = hex::EncodeToString(sha256::Sum256(data));   // "a948904f..."
//     auto [b, err] = hex::DecodeString("48656c6c6f");          // "Hello"
//     fmt::Print("{}", hex::Dump(b));                           // like hexdump -C
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <span>
#include <string_view>
#include <utility>
#endif

namespace gopp::hex {

// hex.ErrLength: the input of Decode had an odd length.
extern const error ErrLength;

// hex.InvalidByteError: the input of Decode had a byte that isn't a hex
// digit.
struct InvalidByteError : errorInterface {
    explicit InvalidByteError(byte b) noexcept : Byte(b) {}
    string Error() const override;
    byte Byte;
};

// The length of the encoding of n bytes: n * 2.
constexpr isize EncodedLen(isize n) noexcept { return n * 2; }
// The length of the decoding of n hex digits: n / 2.
constexpr isize DecodedLen(isize n) noexcept { return n / 2; }

// Writes the EncodedLen(len(src)) lowercase hex digits of src into dst;
// returns their count.
isize Encode(std::span<byte> dst, std::span<const byte> src) noexcept;
// Appends the hex digits of src to dst.
slice<byte> AppendEncode(slice<byte> dst, std::span<const byte> src);
// The lowercase hex digits of src.
string EncodeToString(std::span<const byte> src);

// Decodes the hex digits of src (either case) into dst, which must hold
// DecodedLen(len(src)) bytes. Returns the bytes written and, if src isn't
// valid, an InvalidByteError or ErrLength.
std::pair<isize, error> Decode(std::span<byte> dst, std::span<const byte> src);
// Appends the decoding of src to dst.
std::pair<slice<byte>, error> AppendDecode(slice<byte> dst, std::span<const byte> src);
// The bytes of the hex digits of s.
std::pair<slice<byte>, error> DecodeString(std::string_view s);

// A hex dump of data, as `hexdump -C` writes it.
string Dump(std::span<const byte> data);
// A WriteCloser that writes a hex dump of what's written to w. Close writes
// the last, incomplete line.
ptr<io::WriteCloser> Dumper(ptr<io::Writer> w);

// A Writer that writes the hex digits of what's written to w.
ptr<io::Writer> NewEncoder(ptr<io::Writer> w);
// A Reader of the bytes of the hex digits it reads from r.
ptr<io::Reader> NewDecoder(ptr<io::Reader> r);

}  // namespace gopp::hex
