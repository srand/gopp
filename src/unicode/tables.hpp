// The unexported tables of Go's unicode package, in tables.cpp.
#pragma once

#include <gopp/unicode.hpp>

#include <span>

namespace gopp::unicode::detail {

// The bits of properties.
inline constexpr uint8 pC = 1 << 0;   // a control character.
inline constexpr uint8 pP = 1 << 1;   // a punctuation character.
inline constexpr uint8 pN = 1 << 2;   // a numeral.
inline constexpr uint8 pS = 1 << 3;   // a symbolic character.
inline constexpr uint8 pZ = 1 << 4;   // a spacing character.
inline constexpr uint8 pLu = 1 << 5;  // an upper-case letter.
inline constexpr uint8 pLl = 1 << 6;  // a lower-case letter.
inline constexpr uint8 pp = 1 << 7;   // a printable character according to Go's definition.
inline constexpr uint8 pg = pp | pZ;  // a graphical character according to the Unicode definition.
inline constexpr uint8 pLo = pLl | pLu;  // a letter that is neither upper nor lower case.
inline constexpr uint8 pLmask = pLo;

// The properties of the Latin-1 code points.
extern const uint8 properties[MaxLatin1 + 1];

// SimpleFold of the ASCII code points.
extern const uint16 asciiFold[MaxASCII + 1];

// The code points whose fold orbit has more than two members (or isn't
// just upper and lower case), each with the next in its orbit.
struct foldPair {
    uint16 From;
    uint16 To;
};
extern const std::span<const foldPair> caseOrbit;

}  // namespace gopp::unicode::detail
