// The math/big package, the part that go++'s crypto needs: arbitrary
// precision integers over OpenSSL's BIGNUM.
//
//     big::Int n;
//     auto [_, ok] = n.SetString("ffffffff00000000", 16);
//     fmt::Println("{} bits: {}", n.BitLen(), n.String());
//
// Left out of Go's Int: arithmetic, Format, the Set* and Append* methods
// that nothing here uses, and Bits.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <span>
#include <string_view>
#include <utility>
#endif

struct bignum_st;  // OpenSSL's BIGNUM

namespace gopp::big {

// big.Int: a signed integer of any size. Unlike Go's, it is a value type
// that copies deeply, so `Int z = x` is Go's `z.Set(&x)`.
class Int : public object {
public:
    Int();
    Int(const Int&);
    Int& operator=(const Int&);
    Int(Int&& o) noexcept : bn_(std::exchange(o.bn_, nullptr)) {}
    Int& operator=(Int&& o) noexcept {
        std::swap(bn_, o.bn_);
        return *this;
    }
    ~Int();

    // z.SetInt64(x): sets z to x and returns z.
    Int& SetInt64(int64 x);
    // z.SetBytes(buf): sets z to the unsigned big-endian integer in buf.
    Int& SetBytes(std::span<const byte> buf);
    // z.SetString(s, base): sets z to the value of s in the base, 0 or 2 to
    // 62, and returns {this, true}, or {nullptr, false} if s is not a valid
    // number, leaving z as it was. As in Go, an optional sign comes first;
    // base 0 takes the base from the prefix 0b, 0o, 0x or a leading 0 and
    // allows underscores between digits; the letters a to z are 10 to 35,
    // and A to Z are also 10 to 35 up to base 36, but 36 to 61 beyond it.
    std::pair<Int*, bool> SetString(std::string_view s, int base);

    // x.Bytes(): the absolute value as unsigned big-endian bytes, without
    // leading zeros: empty for zero.
    slice<byte> Bytes() const;
    // x.FillBytes(buf): the absolute value as big-endian bytes in buf, with
    // leading zeros, and returns them as a slice. Panics if buf is too
    // small. Unlike Go's, the returned slice is a copy of buf.
    slice<byte> FillBytes(std::span<byte> buf) const;

    // x.Cmp(y): -1, 0 or +1 for x less than, equal to or greater than y.
    int Cmp(const Int& y) const;
    // x.Sign(): -1, 0 or +1 for a negative, zero or positive x.
    int Sign() const;
    // x.BitLen(): the length of the absolute value in bits; 0 for zero.
    int BitLen() const;
    // x.Int64(): x as an int64, undefined if !IsInt64().
    int64 Int64() const;
    // x.IsInt64(): whether x fits an int64.
    bool IsInt64() const;
    // x.String() and x.Text(base): x in decimal, and in the base 2 to 62
    // with lower case letters up to base 36.
    string String() const;
    string Text(int base) const;

    // The BIGNUM, for go++'s crypto.
    const void* bn() const;

private:
    bignum_st* mut();  // bn_, allocated if x was moved from
    bignum_st* bn_;
};

// big.NewInt: an Int set to x.
ptr<Int> NewInt(int64 x);

}  // namespace gopp::big
