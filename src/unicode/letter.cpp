// Ported from Go 1.27.3's unicode/letter.go, graphic.go, digit.go and
// casetables.go.
#include <gopp/unicode.hpp>

#include "tables.hpp"

namespace gopp::unicode {

using namespace detail;

namespace {

// The maximum size table for linear search for non-Latin1 rune. Derived
// by running 'go test -calibrate'.
constexpr std::size_t linearMax = 18;

// Whether r is in the sorted slice of 16-bit ranges.
bool is16(std::span<const Range16> ranges, uint16 r) noexcept {
    if (ranges.size() <= linearMax || r <= MaxLatin1) {
        for (const auto& range_ : ranges) {
            if (r < range_.Lo) return false;
            if (r <= range_.Hi) return range_.Stride == 1 || (r - range_.Lo) % range_.Stride == 0;
        }
        return false;
    }
    // binary search over ranges
    std::size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        std::size_t m = lo + (hi - lo) / 2;
        const auto& range_ = ranges[m];
        if (range_.Lo <= r && r <= range_.Hi) return range_.Stride == 1 || (r - range_.Lo) % range_.Stride == 0;
        if (r < range_.Lo) {
            hi = m;
        } else {
            lo = m + 1;
        }
    }
    return false;
}

// Whether r is in the sorted slice of 32-bit ranges.
bool is32(std::span<const Range32> ranges, uint32 r) noexcept {
    if (ranges.size() <= linearMax) {
        for (const auto& range_ : ranges) {
            if (r < range_.Lo) return false;
            if (r <= range_.Hi) return range_.Stride == 1 || (r - range_.Lo) % range_.Stride == 0;
        }
        return false;
    }
    // binary search over ranges
    std::size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        std::size_t m = lo + (hi - lo) / 2;
        const auto& range_ = ranges[m];
        if (range_.Lo <= r && r <= range_.Hi) return range_.Stride == 1 || (r - range_.Lo) % range_.Stride == 0;
        if (r < range_.Lo) {
            hi = m;
        } else {
            lo = m + 1;
        }
    }
    return false;
}

bool isExcludingLatin(const RangeTable* rangeTab, rune r) noexcept {
    auto r16 = rangeTab->R16;
    // Compare as uint32 to correctly handle negative runes.
    if (auto off = std::size_t(rangeTab->LatinOffset); r16.size() > off && uint32(r) <= uint32(r16.back().Hi)) {
        return is16(r16.subspan(off), uint16(r));
    }
    auto r32 = rangeTab->R32;
    if (!r32.empty() && r >= rune(r32[0].Lo)) return is32(r32, uint32(r));
    return false;
}

// The CaseRange mapping for rune r or nullptr if no mapping exists for r.
const CaseRange* lookupCaseRange(rune r, std::span<const CaseRange> caseRange) noexcept {
    // binary search over ranges
    std::size_t lo = 0, hi = caseRange.size();
    while (lo < hi) {
        std::size_t m = lo + (hi - lo) / 2;
        const CaseRange* cr = &caseRange[m];
        if (rune(cr->Lo) <= r && r <= rune(cr->Hi)) return cr;
        if (r < rune(cr->Lo)) {
            hi = m;
        } else {
            lo = m + 1;
        }
    }
    return nullptr;
}

// r converted to _case using CaseRange cr.
rune convertCase(int _case, rune r, const CaseRange* cr) noexcept {
    rune delta = cr->Delta[_case];
    if (delta > MaxRune) {
        // In an Upper-Lower sequence, which always starts with an
        // UpperCase letter, the real deltas always look like:
        //	{0, 1, 0}    UpperCase (Lower is next)
        //	{-1, 0, -1}  LowerCase (Upper, Title are previous)
        // The characters at even offsets from the beginning of the
        // sequence are upper case; the ones at odd offsets are lower. The
        // correct mapping can be done by clearing or setting the low bit
        // in the sequence offset. The constants UpperCase and TitleCase
        // are even while LowerCase is odd so we take the low bit from
        // _case.
        return rune(cr->Lo) + (((r - rune(cr->Lo)) & ~rune(1)) | rune(_case & 1));
    }
    return r + delta;
}

// The rune mapped with the case mapping, and whether caseRange contained
// a mapping for r.
std::pair<rune, bool> to(int _case, rune r, std::span<const CaseRange> caseRange) noexcept {
    if (_case < 0 || MaxCase <= _case) return {ReplacementChar, false};  // as reasonable an error as any
    if (auto cr = lookupCaseRange(r, caseRange)) return {convertCase(_case, r, cr), true};
    return {r, false};
}

constexpr CaseRange turkishCase[] = {
    {0x0049, 0x0049, {0, 0x131 - 0x49, 0}},
    {0x0069, 0x0069, {0x130 - 0x69, 0, 0x130 - 0x69}},
    {0x0130, 0x0130, {0, 0x69 - 0x130, 0}},
    {0x0131, 0x0131, {0x49 - 0x131, 0, 0x49 - 0x131}},
};

}  // namespace

constinit const SpecialCase TurkishCase{turkishCase};
constinit const SpecialCase AzeriCase{turkishCase};

bool Is(const RangeTable* rangeTab, rune r) noexcept {
    auto r16 = rangeTab->R16;
    // Compare as uint32 to correctly handle negative runes.
    if (!r16.empty() && uint32(r) <= uint32(r16.back().Hi)) return is16(r16, uint16(r));
    auto r32 = rangeTab->R32;
    if (!r32.empty() && r >= rune(r32[0].Lo)) return is32(r32, uint32(r));
    return false;
}

bool IsOneOf(std::span<const RangeTable* const> ranges, rune r) noexcept {
    for (auto inside : ranges) {
        if (Is(inside, r)) return true;
    }
    return false;
}

bool IsUpper(rune r) noexcept {
    // See comment in IsGraphic.
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pLmask) == pLu;
    return isExcludingLatin(Upper, r);
}

bool IsLower(rune r) noexcept {
    // See comment in IsGraphic.
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pLmask) == pLl;
    return isExcludingLatin(Lower, r);
}

bool IsTitle(rune r) noexcept {
    if (r <= MaxLatin1) return false;
    return isExcludingLatin(Title, r);
}

bool IsGraphic(rune r) noexcept {
    // We convert to uint32 to avoid the extra test for negative, and in
    // the index we convert to uint8 to avoid the range check.
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pg) != 0;
    return IsOneOf(GraphicRanges, r);
}

bool IsPrint(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pp) != 0;
    return IsOneOf(PrintRanges, r);
}

bool IsControl(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pC) != 0;
    // All control characters are < MaxLatin1.
    return false;
}

bool IsLetter(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pLmask) != 0;
    return isExcludingLatin(Letter, r);
}

bool IsMark(rune r) noexcept {
    // There are no mark characters in Latin-1.
    return isExcludingLatin(Mark, r);
}

bool IsNumber(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pN) != 0;
    return isExcludingLatin(Number, r);
}

bool IsPunct(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pP) != 0;
    return Is(Punct, r);
}

bool IsSpace(rune r) noexcept {
    // This property isn't the same as Z; special-case it.
    if (uint32(r) <= MaxLatin1) {
        switch (r) {
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case ' ':
        case 0x85:
        case 0xA0:
            return true;
        }
        return false;
    }
    return isExcludingLatin(White_Space, r);
}

bool IsSymbol(rune r) noexcept {
    if (uint32(r) <= MaxLatin1) return (properties[uint8(r)] & pS) != 0;
    return isExcludingLatin(Symbol, r);
}

bool IsDigit(rune r) noexcept {
    if (r <= MaxLatin1) return '0' <= r && r <= '9';
    return isExcludingLatin(Digit, r);
}

rune To(int _case, rune r) noexcept { return to(_case, r, CaseRanges).first; }

rune ToUpper(rune r) noexcept {
    if (r <= MaxASCII) {
        if ('a' <= r && r <= 'z') r -= 'a' - 'A';
        return r;
    }
    return To(UpperCase, r);
}

rune ToLower(rune r) noexcept {
    if (r <= MaxASCII) {
        if ('A' <= r && r <= 'Z') r += 'a' - 'A';
        return r;
    }
    return To(LowerCase, r);
}

rune ToTitle(rune r) noexcept {
    if (r <= MaxASCII) {
        if ('a' <= r && r <= 'z') r -= 'a' - 'A';  // title case is upper case for ASCII
        return r;
    }
    return To(TitleCase, r);
}

rune SpecialCase::ToUpper(rune r) const noexcept {
    auto [r1, hadMapping] = to(UpperCase, r, ranges);
    if (r1 == r && !hadMapping) r1 = unicode::ToUpper(r);
    return r1;
}

rune SpecialCase::ToTitle(rune r) const noexcept {
    auto [r1, hadMapping] = to(TitleCase, r, ranges);
    if (r1 == r && !hadMapping) r1 = unicode::ToTitle(r);
    return r1;
}

rune SpecialCase::ToLower(rune r) const noexcept {
    auto [r1, hadMapping] = to(LowerCase, r, ranges);
    if (r1 == r && !hadMapping) r1 = unicode::ToLower(r);
    return r1;
}

rune SimpleFold(rune r) noexcept {
    if (r < 0 || r > MaxRune) return r;
    if (r <= MaxASCII) return rune(asciiFold[r]);
    // Consult caseOrbit table for special cases.
    std::size_t lo = 0, hi = caseOrbit.size();
    while (lo < hi) {
        std::size_t m = lo + (hi - lo) / 2;
        if (rune(caseOrbit[m].From) < r) {
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    if (lo < caseOrbit.size() && rune(caseOrbit[lo].From) == r) return rune(caseOrbit[lo].To);
    // No folding specified. This is a one- or two-element equivalence
    // class containing rune and ToLower(rune) and ToUpper(rune) if they
    // are different from rune.
    if (auto cr = lookupCaseRange(r, CaseRanges)) {
        if (rune l = convertCase(LowerCase, r, cr); l != r) return l;
        return convertCase(UpperCase, r, cr);
    }
    return r;
}

}  // namespace gopp::unicode
