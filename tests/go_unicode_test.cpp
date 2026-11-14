// Ported from Go 1.27.3's unicode/letter_test.go, digit_test.go,
// graphic_test.go and script_test.go, with Go's test names.
// Left out: the benchmarks and TestCalibrate.
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>

#include <gtest/gtest.h>

#include <format>
#include <set>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::unicode;

namespace {

struct caseT {
    int cas;
    rune in, out;
};

struct T {
    rune rune_;
    std::string script;
};

std::string U(rune r) { return std::format("U+{:04X}", uint32(r)); }

const std::vector<rune> upperTest = {
    0x41,
    0xc0,
    0xd8,
    0x100,
    0x139,
    0x14a,
    0x178,
    0x181,
    0x376,
    0x3cf,
    0x13bd,
    0x1f2a,
    0x2102,
    0x2c00,
    0x2c10,
    0x2c20,
    0xa650,
    0xa722,
    0xff3a,
    0x10400,
    0x1d400,
    0x1d7ca,
};

const std::vector<rune> notupperTest = {
    0x40,
    0x5b,
    0x61,
    0x185,
    0x1b0,
    0x377,
    0x387,
    0x2150,
    0xab7d,
    0xffff,
    0x10000,
};

const std::vector<rune> letterTest = {
    0x41,
    0x61,
    0xaa,
    0xba,
    0xc8,
    0xdb,
    0xf9,
    0x2ec,
    0x535,
    0x620,
    0x6e6,
    0x93d,
    0xa15,
    0xb99,
    0xdc0,
    0xedd,
    0x1000,
    0x1200,
    0x1312,
    0x1401,
    0x2c00,
    0xa800,
    0xf900,
    0xfa30,
    0xffda,
    0xffdc,
    0x10000,
    0x10300,
    0x10400,
    0x20000,
    0x2f800,
    0x2fa1d,
};

const std::vector<rune> notletterTest = {
    0x20,
    0x35,
    0x375,
    0x619,
    0x700,
    0x1885,
    0xfffe,
    0x1ffff,
    0x10ffff,
};

const std::vector<rune> spaceTest = {
    0x09,
    0x0a,
    0x0b,
    0x0c,
    0x0d,
    0x20,
    0x85,
    0xA0,
    0x2000,
    0x3000,
};

const std::vector<caseT> caseTest = {
    // errors
    {-1, U'\n', 0xFFFD},
    {UpperCase, -1, -1},
    {UpperCase, 1 << 30, 1 << 30},

    // ASCII (special-cased so test carefully)
    {UpperCase, U'\n', U'\n'},
    {UpperCase, U'a', U'A'},
    {UpperCase, U'A', U'A'},
    {UpperCase, U'7', U'7'},
    {LowerCase, U'\n', U'\n'},
    {LowerCase, U'a', U'a'},
    {LowerCase, U'A', U'a'},
    {LowerCase, U'7', U'7'},
    {TitleCase, U'\n', U'\n'},
    {TitleCase, U'a', U'A'},
    {TitleCase, U'A', U'A'},
    {TitleCase, U'7', U'7'},

    // Latin-1: easy to read the tests!
    {UpperCase, 0x80, 0x80},
    {UpperCase, U'Å', U'Å'},
    {UpperCase, U'å', U'Å'},
    {LowerCase, 0x80, 0x80},
    {LowerCase, U'Å', U'å'},
    {LowerCase, U'å', U'å'},
    {TitleCase, 0x80, 0x80},
    {TitleCase, U'Å', U'Å'},
    {TitleCase, U'å', U'Å'},

    // 0131;LATIN SMALL LETTER DOTLESS I;Ll;0;L;;;;;N;;;0049;;0049
    {UpperCase, 0x0131, U'I'},
    {LowerCase, 0x0131, 0x0131},
    {TitleCase, 0x0131, U'I'},

    // 0133;LATIN SMALL LIGATURE IJ;Ll;0;L;<compat> 0069 006A;;;;N;LATIN SMALL LETTER I J;;0132;;0132
    {UpperCase, 0x0133, 0x0132},
    {LowerCase, 0x0133, 0x0133},
    {TitleCase, 0x0133, 0x0132},

    // 212A;KELVIN SIGN;Lu;0;L;004B;;;;N;DEGREES KELVIN;;;006B;
    {UpperCase, 0x212A, 0x212A},
    {LowerCase, 0x212A, U'k'},
    {TitleCase, 0x212A, 0x212A},

    // From an UpperLower sequence
    // A640;CYRILLIC CAPITAL LETTER ZEMLYA;Lu;0;L;;;;;N;;;;A641;
    {UpperCase, 0xA640, 0xA640},
    {LowerCase, 0xA640, 0xA641},
    {TitleCase, 0xA640, 0xA640},
    // A641;CYRILLIC SMALL LETTER ZEMLYA;Ll;0;L;;;;;N;;;A640;;A640
    {UpperCase, 0xA641, 0xA640},
    {LowerCase, 0xA641, 0xA641},
    {TitleCase, 0xA641, 0xA640},
    // A64E;CYRILLIC CAPITAL LETTER NEUTRAL YER;Lu;0;L;;;;;N;;;;A64F;
    {UpperCase, 0xA64E, 0xA64E},
    {LowerCase, 0xA64E, 0xA64F},
    {TitleCase, 0xA64E, 0xA64E},
    // A65F;CYRILLIC SMALL LETTER YN;Ll;0;L;;;;;N;;;A65E;;A65E
    {UpperCase, 0xA65F, 0xA65E},
    {LowerCase, 0xA65F, 0xA65F},
    {TitleCase, 0xA65F, 0xA65E},

    // From another UpperLower sequence
    // 0139;LATIN CAPITAL LETTER L WITH ACUTE;Lu;0;L;004C 0301;;;;N;LATIN CAPITAL LETTER L ACUTE;;;013A;
    {UpperCase, 0x0139, 0x0139},
    {LowerCase, 0x0139, 0x013A},
    {TitleCase, 0x0139, 0x0139},
    // 013F;LATIN CAPITAL LETTER L WITH MIDDLE DOT;Lu;0;L;<compat> 004C 00B7;;;;N;;;;0140;
    {UpperCase, 0x013f, 0x013f},
    {LowerCase, 0x013f, 0x0140},
    {TitleCase, 0x013f, 0x013f},
    // 0148;LATIN SMALL LETTER N WITH CARON;Ll;0;L;006E 030C;;;;N;LATIN SMALL LETTER N HACEK;;0147;;0147
    {UpperCase, 0x0148, 0x0147},
    {LowerCase, 0x0148, 0x0148},
    {TitleCase, 0x0148, 0x0147},

    // Lowercase lower than uppercase.
    // AB78;CHEROKEE SMALL LETTER GE;Ll;0;L;;;;;N;;;13A8;;13A8
    {UpperCase, 0xab78, 0x13a8},
    {LowerCase, 0xab78, 0xab78},
    {TitleCase, 0xab78, 0x13a8},
    {UpperCase, 0x13a8, 0x13a8},
    {LowerCase, 0x13a8, 0xab78},
    {TitleCase, 0x13a8, 0x13a8},

    // Last block in the 5.1.0 table
    // 10400;DESERET CAPITAL LETTER LONG I;Lu;0;L;;;;;N;;;;10428;
    {UpperCase, 0x10400, 0x10400},
    {LowerCase, 0x10400, 0x10428},
    {TitleCase, 0x10400, 0x10400},
    // 10427;DESERET CAPITAL LETTER EW;Lu;0;L;;;;;N;;;;1044F;
    {UpperCase, 0x10427, 0x10427},
    {LowerCase, 0x10427, 0x1044F},
    {TitleCase, 0x10427, 0x10427},
    // 10428;DESERET SMALL LETTER LONG I;Ll;0;L;;;;;N;;;10400;;10400
    {UpperCase, 0x10428, 0x10400},
    {LowerCase, 0x10428, 0x10428},
    {TitleCase, 0x10428, 0x10400},
    // 1044F;DESERET SMALL LETTER EW;Ll;0;L;;;;;N;;;10427;;10427
    {UpperCase, 0x1044F, 0x10427},
    {LowerCase, 0x1044F, 0x1044F},
    {TitleCase, 0x1044F, 0x10427},

    // First one not in the 5.1.0 table
    // 10450;SHAVIAN LETTER PEEP;Lo;0;L;;;;;N;;;;;
    {UpperCase, 0x10450, 0x10450},
    {LowerCase, 0x10450, 0x10450},
    {TitleCase, 0x10450, 0x10450},

    // Non-letters with case.
    {LowerCase, 0x2161, 0x2171},
    {UpperCase, 0x0345, 0x0399},
};

const std::vector<rune> testDigit = {
    0x0030,
    0x0039,
    0x0661,
    0x06F1,
    0x07C9,
    0x0966,
    0x09EF,
    0x0A66,
    0x0AEF,
    0x0B66,
    0x0B6F,
    0x0BE6,
    0x0BEF,
    0x0C66,
    0x0CEF,
    0x0D66,
    0x0D6F,
    0x0E50,
    0x0E59,
    0x0ED0,
    0x0ED9,
    0x0F20,
    0x0F29,
    0x1040,
    0x1049,
    0x1090,
    0x1091,
    0x1099,
    0x17E0,
    0x17E9,
    0x1810,
    0x1819,
    0x1946,
    0x194F,
    0x19D0,
    0x19D9,
    0x1B50,
    0x1B59,
    0x1BB0,
    0x1BB9,
    0x1C40,
    0x1C49,
    0x1C50,
    0x1C59,
    0xA620,
    0xA629,
    0xA8D0,
    0xA8D9,
    0xA900,
    0xA909,
    0xAA50,
    0xAA59,
    0xFF10,
    0xFF19,
    0x104A1,
    0x1D7CE,
};

const std::vector<rune> testLetter = {
    0x0041,
    0x0061,
    0x00AA,
    0x00BA,
    0x00C8,
    0x00DB,
    0x00F9,
    0x02EC,
    0x0535,
    0x06E6,
    0x093D,
    0x0A15,
    0x0B99,
    0x0DC0,
    0x0EDD,
    0x1000,
    0x1200,
    0x1312,
    0x1401,
    0x1885,
    0x2C00,
    0xA800,
    0xF900,
    0xFA30,
    0xFFDA,
    0xFFDC,
    0x10000,
    0x10300,
    0x10400,
    0x20000,
    0x2F800,
    0x2FA1D,
};

const std::vector<T> inCategoryTest = {
    {0x0081, "Cc"},
    {0x200B, "Cf"},
    {0xf0000, "Co"},
    {0xdb80, "Cs"},
    {0x0236, "Ll"},
    {0x1d9d, "Lm"},
    {0x07cf, "Lo"},
    {0x1f8a, "Lt"},
    {0x03ff, "Lu"},
    {0x0bc1, "Mc"},
    {0x20df, "Me"},
    {0x07f0, "Mn"},
    {0x1bb2, "Nd"},
    {0x10147, "Nl"},
    {0x2478, "No"},
    {0xfe33, "Pc"},
    {0x2011, "Pd"},
    {0x301e, "Pe"},
    {0x2e03, "Pf"},
    {0x2e02, "Pi"},
    {0x0022, "Po"},
    {0x2770, "Ps"},
    {0x00a4, "Sc"},
    {0xa711, "Sk"},
    {0x25f9, "Sm"},
    {0x2108, "So"},
    {0x2028, "Zl"},
    {0x2029, "Zp"},
    {0x202f, "Zs"},
    // Unifieds.
    {0x04aa, "L"},
    {0x0009, "C"},
    {0x1712, "M"},
    {0x0031, "N"},
    {0x00bb, "P"},
    {0x00a2, "S"},
    {0x00a0, "Z"},
    {0x0065, "LC"},
    // Unassigned
    {0x0378, "Cn"},
    {0x0378, "C"},
};

const std::vector<T> inPropTest = {
    {0x0046, "ASCII_Hex_Digit"},
    {0x200F, "Bidi_Control"},
    {0x2212, "Dash"},
    {0xE0001, "Deprecated"},
    {0x00B7, "Diacritic"},
    {0x30FE, "Extender"},
    {0xFF46, "Hex_Digit"},
    {0x2E17, "Hyphen"},
    {0x2FFB, "IDS_Binary_Operator"},
    {0x2FF3, "IDS_Trinary_Operator"},
    {0xFA6A, "Ideographic"},
    {0x200D, "Join_Control"},
    {0x0EC4, "Logical_Order_Exception"},
    {0x2FFFF, "Noncharacter_Code_Point"},
    {0x065E, "Other_Alphabetic"},
    {0x2065, "Other_Default_Ignorable_Code_Point"},
    {0x0BD7, "Other_Grapheme_Extend"},
    {0x0387, "Other_ID_Continue"},
    {0x212E, "Other_ID_Start"},
    {0x2094, "Other_Lowercase"},
    {0x2040, "Other_Math"},
    {0x216F, "Other_Uppercase"},
    {0x0027, "Pattern_Syntax"},
    {0x0020, "Pattern_White_Space"},
    {0x06DD, "Prepended_Concatenation_Mark"},
    {0x300D, "Quotation_Mark"},
    {0x2EF3, "Radical"},
    {0x1f1ff, "Regional_Indicator"},
    {0x061F, "STerm"}, // Deprecated alias of Sentence_Terminal
    {0x061F, "Sentence_Terminal"},
    {0x2071, "Soft_Dotted"},
    {0x003A, "Terminal_Punctuation"},
    {0x9FC3, "Unified_Ideograph"},
    {0xFE0F, "Variation_Selector"},
    {0x0020, "White_Space"},
    {0x221e, "ID_Compat_Math_Start"},
    {0x06e3, "Modifier_Combining_Mark"},
    {0x2080, "ID_Compat_Math_Continue"},
    {0x2ffe, "IDS_Unary_Operator"},
};

TEST(GoUnicode, IsLetter) {
    for (rune r : upperTest) EXPECT_TRUE(IsLetter(r)) << "IsLetter(" << U(r) << ") = false, want true";
    for (rune r : letterTest) EXPECT_TRUE(IsLetter(r)) << "IsLetter(" << U(r) << ") = false, want true";
    for (rune r : notletterTest) EXPECT_FALSE(IsLetter(r)) << "IsLetter(" << U(r) << ") = true, want false";
}

TEST(GoUnicode, IsUpper) {
    for (rune r : upperTest) EXPECT_TRUE(IsUpper(r)) << "IsUpper(" << U(r) << ") = false, want true";
    for (rune r : notupperTest) EXPECT_FALSE(IsUpper(r)) << "IsUpper(" << U(r) << ") = true, want false";
    for (rune r : notletterTest) EXPECT_FALSE(IsUpper(r)) << "IsUpper(" << U(r) << ") = true, want false";
}

std::string caseString(int c) {
    switch (c) {
    case UpperCase:
        return "UpperCase";
    case LowerCase:
        return "LowerCase";
    case TitleCase:
        return "TitleCase";
    }
    return "ErrorCase";
}

TEST(GoUnicode, To) {
    for (const auto& c : caseTest) {
        rune r = To(c.cas, c.in);
        EXPECT_EQ(c.out, r) << "To(" << U(c.in) << ", " << caseString(c.cas) << ") = " << U(r) << " want " << U(c.out);
    }
}

TEST(GoUnicode, ToUpperCase) {
    for (const auto& c : caseTest) {
        if (c.cas != UpperCase) continue;
        rune r = ToUpper(c.in);
        EXPECT_EQ(c.out, r) << "ToUpper(" << U(c.in) << ") = " << U(r) << " want " << U(c.out);
    }
}

TEST(GoUnicode, ToLowerCase) {
    for (const auto& c : caseTest) {
        if (c.cas != LowerCase) continue;
        rune r = ToLower(c.in);
        EXPECT_EQ(c.out, r) << "ToLower(" << U(c.in) << ") = " << U(r) << " want " << U(c.out);
    }
}

TEST(GoUnicode, ToTitleCase) {
    for (const auto& c : caseTest) {
        if (c.cas != TitleCase) continue;
        rune r = ToTitle(c.in);
        EXPECT_EQ(c.out, r) << "ToTitle(" << U(c.in) << ") = " << U(r) << " want " << U(c.out);
    }
}

TEST(GoUnicode, IsSpace) {
    for (rune c : spaceTest) EXPECT_TRUE(IsSpace(c)) << "IsSpace(" << U(c) << ") = false; want true";
    for (rune c : letterTest) EXPECT_FALSE(IsSpace(c)) << "IsSpace(" << U(c) << ") = true; want false";
}

// Check that the optimizations for IsLetter etc. agree with the tables.
// We only need to check the Latin-1 range.
TEST(GoUnicode, LetterOptimizations) {
    for (rune i = 0; i <= MaxLatin1; i++) {
        EXPECT_EQ(Is(Letter, i), IsLetter(i)) << "IsLetter(" << U(i) << ") disagrees with Is(Letter)";
        EXPECT_EQ(Is(Upper, i), IsUpper(i)) << "IsUpper(" << U(i) << ") disagrees with Is(Upper)";
        EXPECT_EQ(Is(Lower, i), IsLower(i)) << "IsLower(" << U(i) << ") disagrees with Is(Lower)";
        EXPECT_EQ(Is(Title, i), IsTitle(i)) << "IsTitle(" << U(i) << ") disagrees with Is(Title)";
        EXPECT_EQ(Is(White_Space, i), IsSpace(i)) << "IsSpace(" << U(i) << ") disagrees with Is(White_Space)";
        EXPECT_EQ(To(UpperCase, i), ToUpper(i)) << "ToUpper(" << U(i) << ") disagrees with To(Upper)";
        EXPECT_EQ(To(LowerCase, i), ToLower(i)) << "ToLower(" << U(i) << ") disagrees with To(Lower)";
        EXPECT_EQ(To(TitleCase, i), ToTitle(i)) << "ToTitle(" << U(i) << ") disagrees with To(Title)";
    }
}

TEST(GoUnicode, TurkishCase) {
    std::u32string lower = U"abcçdefgğhıijklmnoöprsştuüvyz";
    std::u32string upper = U"ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ";
    for (std::size_t i = 0; i < lower.size(); i++) {
        rune l = rune(lower[i]), u = rune(upper[i]);
        EXPECT_EQ(TurkishCase.ToLower(l), l) << "lower(" << U(l) << ")";
        EXPECT_EQ(TurkishCase.ToUpper(u), u) << "upper(" << U(u) << ")";
        EXPECT_EQ(TurkishCase.ToUpper(l), u) << "upper(" << U(l) << ")";
        EXPECT_EQ(TurkishCase.ToLower(u), l) << "lower(" << U(u) << ")";
        EXPECT_EQ(TurkishCase.ToTitle(u), u) << "title(" << U(u) << ")";
        EXPECT_EQ(TurkishCase.ToTitle(l), u) << "title(" << U(l) << ")";
    }
}

const std::vector<std::u32string> simpleFoldTests = {
    // SimpleFold(x) returns the next equivalent rune > x or wraps around
    // to smaller values.

    // Easy cases.
    U"Aa",
    U"δΔ",

    // ASCII special cases.
    U"Kk\u212A",  // K, k, KELVIN SIGN
    U"Ssſ",

    // Non-ASCII special cases.
    U"ρϱΡ",
    U"\u0345\u0399\u03B9\u1FBE",  // COMBINING GREEK YPOGEGRAMMENI, Ι, ι, GREEK PROSGEGRAMMENI

    // Extra special cases: has lower/upper but no case fold.
    U"İ",
    U"ı",

    // Upper comes before lower (Cherokee).
    U"Ꮀꮀ",
};

TEST(GoUnicode, SimpleFold) {
    for (const auto& cycle : simpleFoldTests) {
        rune r = rune(cycle.back());
        for (char32_t c : cycle) {
            rune out = rune(c);
            EXPECT_EQ(SimpleFold(r), out) << "SimpleFold(" << U(r) << ")";
            r = out;
        }
    }
    EXPECT_EQ(SimpleFold(-42), -42);
}

TEST(GoUnicode, LatinOffset) {
    for (const auto* m : {&Categories, &FoldCategory, &FoldScript, &Properties, &Scripts}) {
        for (auto [name, tab] : *m) {
            int i = 0;
            while (std::size_t(i) < tab->R16.size() && tab->R16[std::size_t(i)].Hi <= MaxLatin1) i++;
            EXPECT_EQ(tab->LatinOffset, i) << name;
        }
    }
}

TEST(GoUnicode, SpecialCaseNoMapping) {
    // Issue 25636
    // no change for rune 'A', zero delta, under upper/lower/title case change.
    const CaseRange noChangeForCapitalA[] = {{'A', 'A', {0, 0, 0}}};
    auto got = strings::ToLowerSpecial(SpecialCase{noChangeForCapitalA}, "ABC");
    EXPECT_EQ(got, "Abc");
}

TEST(GoUnicode, NegativeRune) {
    // Issue 43254
    // These tests cover negative rune handling by testing values which,
    // when cast to uint8 or uint16, look like a particular valid rune.
    // This package has Latin-1-specific optimizations, so we test all of
    // Latin-1 and representative non-Latin-1 values in the character
    // categories covered by IsGraphic, etc.
    const std::vector<uint32> nonLatin1 = {
        0x0100,  // Lu: LATIN CAPITAL LETTER A WITH MACRON
        0x0101,  // Ll: LATIN SMALL LETTER A WITH MACRON
        0x01C5,  // Lt: LATIN CAPITAL LETTER D WITH SMALL LETTER Z WITH CARON
        0x0300,  // M: COMBINING GRAVE ACCENT
        0x0660,  // Nd: ARABIC-INDIC DIGIT ZERO
        0x037E,  // P: GREEK QUESTION MARK
        0x02C2,  // S: MODIFIER LETTER LEFT ARROWHEAD
        0x1680,  // Z: OGHAM SPACE MARK
    };
    for (std::size_t i = 0; i < std::size_t(MaxLatin1) + nonLatin1.size(); i++) {
        uint32 base = uint32(i);
        if (i >= std::size_t(MaxLatin1)) base = nonLatin1[i - std::size_t(MaxLatin1)];
        // Note r is negative, but uint8(r) == uint8(base) and
        // uint16(r) == uint16(base).
        rune r = rune(base - (uint32(1) << 31));
        EXPECT_FALSE(Is(Letter, r)) << std::hex << base;
        EXPECT_FALSE(IsControl(r)) << std::hex << base;
        EXPECT_FALSE(IsDigit(r)) << std::hex << base;
        EXPECT_FALSE(IsGraphic(r)) << std::hex << base;
        EXPECT_FALSE(IsLetter(r)) << std::hex << base;
        EXPECT_FALSE(IsLower(r)) << std::hex << base;
        EXPECT_FALSE(IsMark(r)) << std::hex << base;
        EXPECT_FALSE(IsNumber(r)) << std::hex << base;
        EXPECT_FALSE(IsPrint(r)) << std::hex << base;
        EXPECT_FALSE(IsPunct(r)) << std::hex << base;
        EXPECT_FALSE(IsSpace(r)) << std::hex << base;
        EXPECT_FALSE(IsSymbol(r)) << std::hex << base;
        EXPECT_FALSE(IsTitle(r)) << std::hex << base;
        EXPECT_FALSE(IsUpper(r)) << std::hex << base;
    }
}

TEST(GoUnicode, Digit) {
    for (rune r : testDigit) EXPECT_TRUE(IsDigit(r)) << "IsDigit(" << U(r) << ") = false, want true";
    for (rune r : testLetter) EXPECT_FALSE(IsDigit(r)) << "IsDigit(" << U(r) << ") = true, want false";
}

// Test that the special case in IsDigit agrees with the table
TEST(GoUnicode, DigitOptimization) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(Is(Digit, i), IsDigit(i)) << "IsDigit(" << U(i) << ") disagrees with Is(Digit)";
}

// Independently check that the special "Is" functions work in the
// Latin-1 range through the property table.

TEST(GoUnicode, IsControlLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) {
        bool want = (0x00 <= i && i <= 0x1F) || (0x7F <= i && i <= 0x9F);
        EXPECT_EQ(IsControl(i), want) << U(i);
    }
}

TEST(GoUnicode, IsLetterLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsLetter(i), Is(Letter, i)) << U(i);
}

TEST(GoUnicode, IsUpperLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsUpper(i), Is(Upper, i)) << U(i);
}

TEST(GoUnicode, IsLowerLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsLower(i), Is(Lower, i)) << U(i);
}

TEST(GoUnicode, NumberLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsNumber(i), Is(Number, i)) << U(i);
}

TEST(GoUnicode, IsPrintLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) {
        bool want = IsOneOf(PrintRanges, i);
        if (i == ' ') want = true;
        EXPECT_EQ(IsPrint(i), want) << U(i);
    }
}

TEST(GoUnicode, IsGraphicLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsGraphic(i), IsOneOf(GraphicRanges, i)) << U(i);
}

TEST(GoUnicode, IsPunctLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsPunct(i), Is(Punct, i)) << U(i);
}

TEST(GoUnicode, IsSpaceLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsSpace(i), Is(White_Space, i)) << U(i);
}

TEST(GoUnicode, IsSymbolLatin1) {
    for (rune i = 0; i <= MaxLatin1; i++) EXPECT_EQ(IsSymbol(i), Is(Symbol, i)) << U(i);
}

TEST(GoUnicode, Categories) {
    std::set<std::string_view> notTested;
    for (auto [k, _] : Categories) notTested.insert(k);
    for (const auto& test : inCategoryTest) {
        ASSERT_TRUE(Categories.contains(test.script)) << test.script << " not a known category";
        EXPECT_TRUE(Is(Categories[test.script], test.rune_)) << "IsCategory(" << U(test.rune_) << ", " << test.script << ") = false, want true";
        notTested.erase(test.script);
    }
    for (auto k : notTested) ADD_FAILURE() << "category not tested: " << k;
}

TEST(GoUnicode, Properties) {
    std::set<std::string_view> notTested;
    for (auto [k, _] : Properties) notTested.insert(k);
    for (const auto& test : inPropTest) {
        ASSERT_TRUE(Properties.contains(test.script)) << test.script << " not a known prop";
        EXPECT_TRUE(Is(Properties[test.script], test.rune_)) << "IsCategory(" << U(test.rune_) << ", " << test.script << ") = false, want true";
        notTested.erase(test.script);
    }
    for (auto k : notTested) ADD_FAILURE() << "property not tested: " << k;
}

// Not in Go's tests: the maps and the In forms.
TEST(GoUnicode, Maps) {
    EXPECT_EQ(Scripts["Greek"], Greek);
    EXPECT_EQ(Scripts["Klingon"], nullptr);
    EXPECT_EQ(CategoryAliases["Letter"], "L");
    EXPECT_EQ(CategoryAliases["Nope"], "");
    EXPECT_TRUE(In(U'7', Letter, Digit));
    EXPECT_FALSE(In(U'-', Letter, Digit));
    EXPECT_EQ(Version, "17.0.0");
}

}  // namespace
