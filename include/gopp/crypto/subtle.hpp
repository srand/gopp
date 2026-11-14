// The crypto/subtle package: functions whose time doesn't depend on the
// values they're given, for code that handles secrets.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>

#include <span>
#endif

namespace gopp::subtle {

// 1 if x and y are equal, else 0, in time that depends only on their
// lengths. Slices of different lengths give 0 at once.
int ConstantTimeCompare(std::span<const byte> x, std::span<const byte> y) noexcept;
// x if v is 1, y if v is 0.
int ConstantTimeSelect(int v, int x, int y) noexcept;
// 1 if x == y, else 0.
int ConstantTimeByteEq(uint8 x, uint8 y) noexcept;
// 1 if x == y, else 0.
int ConstantTimeEq(int32 x, int32 y) noexcept;
// Copies y into x if v is 1, leaves x if v is 0; x and y must have the
// same length (it panics otherwise).
void ConstantTimeCopy(int v, std::span<byte> x, std::span<const byte> y);
// 1 if x <= y, else 0.
int ConstantTimeLessOrEq(isize x, isize y) noexcept;
// dst[i] = x[i] ^ y[i] for i < min(len(x), len(y)), returning that
// count. It panics if dst is shorter, or overlaps x or y other than
// exactly.
isize XORBytes(std::span<byte> dst, std::span<const byte> x, std::span<const byte> y);

}  // namespace gopp::subtle
