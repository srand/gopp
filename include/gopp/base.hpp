// Basic types shared by all packages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gopp {

using byte = std::uint8_t;
using rune = std::int32_t;
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using uintptr = std::uintptr_t;

// Go's int: the size of a pointer.
using isize = std::ptrdiff_t;

// Go's struct{}: the element type of channels used for signalling.
struct empty {
    friend constexpr bool operator==(empty, empty) noexcept { return true; }
};

// Stops the program with a message, like an unrecovered Go panic.
[[noreturn]] void panic(std::string_view msg) noexcept;

}  // namespace gopp
