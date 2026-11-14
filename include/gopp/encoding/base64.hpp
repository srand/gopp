// The encoding/base64 package: base64 encoding (RFC 4648), ported from Go
// 1.27.3.
//
//     string s = base64::StdEncoding.EncodeToString(data);
//     auto [b, err] = base64::URLEncoding.DecodeString(s);
//     auto raw = base64::StdEncoding.WithPadding(base64::NoPadding);
//
// The encodings are constexpr values: ready before any static initializer
// runs.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <array>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#endif

namespace gopp::base64 {

// The padding characters for WithPadding.
inline constexpr rune StdPadding = '=';
inline constexpr rune NoPadding = -1;

// base64.CorruptInputError: the input of Decode isn't valid base64 at the
// byte Offset.
struct CorruptInputError : errorInterface {
    explicit CorruptInputError(int64 offset) noexcept : Offset(offset) {}
    string Error() const override;
    int64 Offset;
};

// base64.Encoding: a radix 64 encoding, defined by a 64-character alphabet.
class Encoding {
public:
    // base64.NewEncoding: the alphabet must have 64 distinct bytes, none of
    // them '\n' or '\r'. The padding is StdPadding.
    constexpr explicit Encoding(std::string_view encoder) {
        if (encoder.size() != 64) panic("encoding alphabet is not 64-bytes long");
        decodeMap_.fill(invalidIndex);
        for (std::size_t i = 0; i < encoder.size(); i++) {
            auto c = static_cast<byte>(encoder[i]);
            if (c == '\n' || c == '\r') panic("encoding alphabet contains newline character");
            if (decodeMap_[c] != invalidIndex) panic("encoding alphabet includes duplicate symbols");
            encode_[i] = c;
            decodeMap_[c] = static_cast<byte>(i);
        }
    }

    // A copy with padding character padding, or NoPadding.
    constexpr Encoding WithPadding(rune padding) const {
        if (padding < NoPadding || padding == '\r' || padding == '\n' || padding > 0xff) panic("invalid padding");
        if (padding != NoPadding && decodeMap_[static_cast<byte>(padding)] != invalidIndex) {
            panic("padding contained in alphabet");
        }
        Encoding e = *this;
        e.padChar_ = padding;
        return e;
    }

    // A copy that rejects encodings whose unused trailing bits aren't zero
    // (RFC 4648 section 3.5).
    constexpr Encoding Strict() const {
        Encoding e = *this;
        e.strict_ = true;
        return e;
    }

    // The length of the encoding of n bytes.
    constexpr isize EncodedLen(isize n) const noexcept {
        if (padChar_ == NoPadding) return n / 3 * 4 + (n % 3 * 8 + 5) / 6;  // minimum # chars at 6 bits per char
        return (n + 2) / 3 * 4;  // minimum # 4-char quanta, 3 bytes each
    }
    // The most bytes n characters decode to.
    constexpr isize DecodedLen(isize n) const noexcept { return decodedLen(n, padChar_); }

    // Writes the EncodedLen(len(src)) characters of src into dst.
    void Encode(std::span<byte> dst, std::span<const byte> src) const noexcept;
    slice<byte> AppendEncode(slice<byte> dst, std::span<const byte> src) const;
    string EncodeToString(std::span<const byte> src) const;

    // Decodes src into dst, which must hold DecodedLen(len(src)) bytes;
    // returns the bytes written. New lines in src are ignored. On invalid
    // input, the error is a CorruptInputError.
    std::pair<isize, error> Decode(std::span<byte> dst, std::span<const byte> src) const;
    std::pair<slice<byte>, error> AppendDecode(slice<byte> dst, std::span<const byte> src) const;
    std::pair<slice<byte>, error> DecodeString(std::string_view s) const;

    // The padding character, or NoPadding.
    constexpr rune padChar() const noexcept { return padChar_; }

private:
    static constexpr byte invalidIndex = 0xff;

    static constexpr isize decodedLen(isize n, rune padChar) noexcept {
        if (padChar == NoPadding) return n / 4 * 3 + n % 4 * 6 / 8;  // unpadded data may end with partial block of 2-3 characters
        return n / 4 * 3;  // padded base64 should always be a multiple of 4 characters in length
    }
    std::tuple<isize, isize, error> decodeQuantum(std::span<byte> dst, std::span<const byte> src, isize si) const;

    std::array<byte, 64> encode_{};
    std::array<byte, 256> decodeMap_{};
    rune padChar_ = StdPadding;
    bool strict_ = false;
};

// base64.NewEncoding
constexpr Encoding NewEncoding(std::string_view encoder) { return Encoding(encoder); }

// The standard encoding of RFC 4648, with padding.
inline constexpr Encoding StdEncoding{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
// The URL and file name safe encoding of RFC 4648, with padding.
inline constexpr Encoding URLEncoding{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
// StdEncoding without padding.
inline constexpr Encoding RawStdEncoding = StdEncoding.WithPadding(NoPadding);
// URLEncoding without padding.
inline constexpr Encoding RawURLEncoding = URLEncoding.WithPadding(NoPadding);

// base64.NewEncoder: a WriteCloser that writes the encoding of what's
// written to w. Close writes the last, partial block.
ptr<io::WriteCloser> NewEncoder(const Encoding& enc, ptr<io::Writer> w);
// base64.NewDecoder: a Reader of the decoding of what it reads from r.
ptr<io::Reader> NewDecoder(const Encoding& enc, ptr<io::Reader> r);

}  // namespace gopp::base64
