// crypto/subtle, after Go 1.27.3's crypto/subtle and
// crypto/internal/fips140/subtle. The comparisons are computed with
// arithmetic, not branches.
#include <gopp/crypto/subtle.hpp>

#include <algorithm>

namespace gopp::subtle {

namespace {

// 1 if x is zero, else 0.
inline uint64 is_zero(uint64 x) noexcept { return ((x | (0 - x)) >> 63) ^ 1; }

// Keeps the compiler from turning v back into a branch.
inline uint64 opaque(uint64 v) noexcept {
    asm("" : "+r"(v));
    return v;
}

}  // namespace

int ConstantTimeCompare(std::span<const byte> x, std::span<const byte> y) noexcept {
    if (x.size() != y.size()) return 0;
    byte v = 0;
    for (std::size_t i = 0; i < x.size(); i++) v |= x[i] ^ y[i];
    return ConstantTimeByteEq(v, 0);
}

int ConstantTimeSelect(int v, int x, int y) noexcept {
    // mask is all ones if v != 0.
    uint64 mask = 0 - (opaque(is_zero(static_cast<uint64>(static_cast<int64>(v)))) ^ 1);
    return static_cast<int>((static_cast<uint64>(static_cast<int64>(x)) & mask) |
                            (static_cast<uint64>(static_cast<int64>(y)) & ~mask));
}

int ConstantTimeByteEq(uint8 x, uint8 y) noexcept { return static_cast<int>(opaque(is_zero(x ^ y))); }

int ConstantTimeEq(int32 x, int32 y) noexcept {
    return static_cast<int>(opaque(is_zero(static_cast<uint32>(x) ^ static_cast<uint32>(y))));
}

void ConstantTimeCopy(int v, std::span<byte> x, std::span<const byte> y) {
    if (x.size() != y.size()) panic("subtle: slices have different lengths");
    auto xmask = static_cast<byte>(v - 1);
    auto ymask = static_cast<byte>(~(v - 1));
    for (std::size_t i = 0; i < x.size(); i++) x[i] = static_cast<byte>((x[i] & xmask) | (y[i] & ymask));
}

int ConstantTimeLessOrEq(isize x, isize y) noexcept {
    // Flipping the sign bits orders the values as unsigned; then x <= y if
    // y - x doesn't borrow (bits.Sub64's borrow formula).
    uint64 ux = static_cast<uint64>(x) ^ (uint64(1) << 63);
    uint64 uy = static_cast<uint64>(y) ^ (uint64(1) << 63);
    uint64 borrow = ((~uy & ux) | (~(uy ^ ux) & (uy - ux))) >> 63;
    return static_cast<int>(opaque(borrow ^ 1));
}

isize XORBytes(std::span<byte> dst, std::span<const byte> x, std::span<const byte> y) {
    std::size_t n = std::min(x.size(), y.size());
    if (n == 0) return 0;
    if (n > dst.size()) panic("subtle.XORBytes: dst too short");
    // Overlap is allowed only when exact.
    auto inexact_overlap = [n](const byte* a, const byte* b) {
        if (a == b) return false;
        return a <= b + n - 1 && b <= a + n - 1;
    };
    if (inexact_overlap(dst.data(), x.data()) || inexact_overlap(dst.data(), y.data())) {
        panic("subtle.XORBytes: invalid overlap");
    }
    for (std::size_t i = 0; i < n; i++) dst[i] = x[i] ^ y[i];
    return static_cast<isize>(n);
}

}  // namespace gopp::subtle
