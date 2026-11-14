// Ported from Go 1.27.3's regexp/syntax/parse_test.go, simplify_test.go and
// prog_test.go, and regexp/onepass_test.go's TestRunOnePass, with Go's test
// names. Left out: the tests of unexported functions (appendRange,
// mergeRuneSets, compileOnePass), and the benchmarks.
#include <gopp/regexp.hpp>
#include <gopp/regexp/syntax.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <format>
#include <functional>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::syntax;

namespace {

std::string repeat(std::string_view s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

void writeRune(std::string& b, rune r) {
    char buf[4];
    auto n = utf8::EncodeRune(std::span<byte>(reinterpret_cast<byte*>(buf), 4), r);
    b.append(buf, std::size_t(n));
}

const char* opNames(Op op) {
    switch (op) {
    case OpNoMatch: return "no";
    case OpEmptyMatch: return "emp";
    case OpLiteral: return "lit";
    case OpCharClass: return "cc";
    case OpAnyCharNotNL: return "dnl";
    case OpAnyChar: return "dot";
    case OpBeginLine: return "bol";
    case OpEndLine: return "eol";
    case OpBeginText: return "bot";
    case OpEndText: return "eot";
    case OpWordBoundary: return "wb";
    case OpNoWordBoundary: return "nwb";
    case OpCapture: return "cap";
    case OpStar: return "star";
    case OpPlus: return "plus";
    case OpQuest: return "que";
    case OpRepeat: return "rep";
    case OpConcat: return "cat";
    case OpAlternate: return "alt";
    }
    return nullptr;
}

std::string hexOf(rune r) { return r == 0 ? "0x0" : std::format("{:#x}", r); }

// dumpRegexp writes an encoding of the syntax tree for the regexp re to b.
// It is used during testing to distinguish between parses that might print
// the same using re's String method.
void dumpRegexp(std::string& b, const Regexp& re) {
    if (!opNames(re.Op)) {
        b += "op" + std::to_string(int(re.Op));
    } else {
        switch (re.Op) {
        default:
            b += opNames(re.Op);
            break;
        case OpStar:
        case OpPlus:
        case OpQuest:
        case OpRepeat:
            if (re.Flags & NonGreedy) b += 'n';
            b += opNames(re.Op);
            break;
        case OpLiteral:
            b += re.Rune.size() > 1 ? "str" : "lit";
            if (re.Flags & FoldCase) {
                for (rune r : re.Rune) {
                    if (unicode::SimpleFold(r) != r) {
                        b += "fold";
                        break;
                    }
                }
            }
            break;
        }
    }
    b += '{';
    switch (re.Op) {
    case OpEndText:
        if (!(re.Flags & WasDollar)) b += "\\z";
        break;
    case OpLiteral:
        for (rune r : re.Rune) writeRune(b, r);
        break;
    case OpConcat:
    case OpAlternate:
        for (const auto& sub : re.Sub) dumpRegexp(b, *sub);
        break;
    case OpStar:
    case OpPlus:
    case OpQuest:
        dumpRegexp(b, *re.Sub[0]);
        break;
    case OpRepeat:
        b += std::to_string(re.Min) + "," + std::to_string(re.Max) + " ";
        dumpRegexp(b, *re.Sub[0]);
        break;
    case OpCapture:
        if (!re.Name.empty()) {
            b += re.Name.view();
            b += ':';
        }
        dumpRegexp(b, *re.Sub[0]);
        break;
    case OpCharClass: {
        std::string sep;
        for (std::size_t i = 0; i < re.Rune.size(); i += 2) {
            b += sep;
            sep = " ";
            rune lo = re.Rune[i], hi = re.Rune[i + 1];
            if (lo == hi) {
                b += hexOf(lo);
            } else {
                b += hexOf(lo) + "-" + hexOf(hi);
            }
        }
        break;
    }
    default:
        break;
    }
    b += '}';
}

// dump prints a string representation of the regexp showing the structure
// explicitly.
std::string dump(const Regexp& re) {
    std::string b;
    dumpRegexp(b, re);
    return b;
}

std::string mkCharClass(std::function<bool(rune)> f) {
    Regexp re(OpCharClass);
    rune lo = -1;
    for (rune i = 0; i <= unicode::MaxRune; i++) {
        if (f(i)) {
            if (lo < 0) lo = i;
        } else if (lo >= 0) {
            re.Rune.push_back(lo);
            re.Rune.push_back(i - 1);
            lo = -1;
        }
    }
    if (lo >= 0) {
        re.Rune.push_back(lo);
        re.Rune.push_back(unicode::MaxRune);
    }
    return dump(re);
}

bool isUpperFold(rune r) {
    if (unicode::IsUpper(r)) return true;
    for (rune c = unicode::SimpleFold(r); c != r; c = unicode::SimpleFold(c)) {
        if (unicode::IsUpper(c)) return true;
    }
    return false;
}

struct parseTest {
    std::string Regexp;
    std::string Dump;
};

const std::vector<parseTest>& parseTests() {
    static const std::vector<parseTest> tests = {
	// Base cases
	{std::string("a", 1), std::string("lit{a}", 6)},
	{std::string("a.", 2), std::string("cat{lit{a}dot{}}", 16)},
	{std::string("a.b", 3), std::string("cat{lit{a}dot{}lit{b}}", 22)},
	{std::string("ab", 2), std::string("str{ab}", 7)},
	{std::string("a.b.c", 5), std::string("cat{lit{a}dot{}lit{b}dot{}lit{c}}", 33)},
	{std::string("abc", 3), std::string("str{abc}", 8)},
	{std::string("a|^", 3), std::string("alt{lit{a}bol{}}", 16)},
	{std::string("a|b", 3), std::string("cc{0x61-0x62}", 13)},
	{std::string("(a)", 3), std::string("cap{lit{a}}", 11)},
	{std::string("(a)|b", 5), std::string("alt{cap{lit{a}}lit{b}}", 22)},
	{std::string("a*", 2), std::string("star{lit{a}}", 12)},
	{std::string("a+", 2), std::string("plus{lit{a}}", 12)},
	{std::string("a\077", 2), std::string("que{lit{a}}", 11)},
	{std::string("a{2}", 4), std::string("rep{2,2 lit{a}}", 15)},
	{std::string("a{2,3}", 6), std::string("rep{2,3 lit{a}}", 15)},
	{std::string("a{2,}", 5), std::string("rep{2,-1 lit{a}}", 16)},
	{std::string("a*\077", 3), std::string("nstar{lit{a}}", 13)},
	{std::string("a+\077", 3), std::string("nplus{lit{a}}", 13)},
	{std::string("a\077\077", 3), std::string("nque{lit{a}}", 12)},
	{std::string("a{2}\077", 5), std::string("nrep{2,2 lit{a}}", 16)},
	{std::string("a{2,3}\077", 7), std::string("nrep{2,3 lit{a}}", 16)},
	{std::string("a{2,}\077", 6), std::string("nrep{2,-1 lit{a}}", 17)},
	// Malformed { } are treated as literals.
	{std::string("x{1001", 6), std::string("str{x{1001}", 11)},
	{std::string("x{9876543210", 12), std::string("str{x{9876543210}", 17)},
	{std::string("x{9876543210,", 13), std::string("str{x{9876543210,}", 18)},
	{std::string("x{2,1", 5), std::string("str{x{2,1}", 10)},
	{std::string("x{1,9876543210", 14), std::string("str{x{1,9876543210}", 19)},
	{std::string("", 0), std::string("emp{}", 5)},
	{std::string("|", 1), std::string("emp{}", 5)}, // alt{emp{}emp{}} but got factored
	{std::string("|x|", 3), std::string("alt{emp{}lit{x}emp{}}", 21)},
	{std::string(".", 1), std::string("dot{}", 5)},
	{std::string("^", 1), std::string("bol{}", 5)},
	{std::string("$", 1), std::string("eol{}", 5)},
	{std::string("\134|", 2), std::string("lit{|}", 6)},
	{std::string("\134(", 2), std::string("lit{(}", 6)},
	{std::string("\134)", 2), std::string("lit{)}", 6)},
	{std::string("\134*", 2), std::string("lit{*}", 6)},
	{std::string("\134+", 2), std::string("lit{+}", 6)},
	{std::string("\134\077", 2), std::string("lit{\077}", 6)},
	{std::string("{", 1), std::string("lit{{}", 6)},
	{std::string("}", 1), std::string("lit{}}", 6)},
	{std::string("\134.", 2), std::string("lit{.}", 6)},
	{std::string("\134^", 2), std::string("lit{^}", 6)},
	{std::string("\134$", 2), std::string("lit{$}", 6)},
	{std::string("\134\134", 2), std::string("lit{\134}", 6)},
	{std::string("[ace]", 5), std::string("cc{0x61 0x63 0x65}", 18)},
	{std::string("[abc]", 5), std::string("cc{0x61-0x63}", 13)},
	{std::string("[a-z]", 5), std::string("cc{0x61-0x7a}", 13)},
	{std::string("[a]", 3), std::string("lit{a}", 6)},
	{std::string("\134-", 2), std::string("lit{-}", 6)},
	{std::string("-", 1), std::string("lit{-}", 6)},
	{std::string("\134_", 2), std::string("lit{_}", 6)},
	{std::string("abc", 3), std::string("str{abc}", 8)},
	{std::string("abc|def", 7), std::string("alt{str{abc}str{def}}", 21)},
	{std::string("abc|def|ghi", 11), std::string("alt{str{abc}str{def}str{ghi}}", 29)},

	// Posix and Perl extensions
	{std::string("[[:lower:]]", 11), std::string("cc{0x61-0x7a}", 13)},
	{std::string("[a-z]", 5), std::string("cc{0x61-0x7a}", 13)},
	{std::string("[^[:lower:]]", 12), std::string("cc{0x0-0x60 0x7b-0x10ffff}", 26)},
	{std::string("[[:^lower:]]", 12), std::string("cc{0x0-0x60 0x7b-0x10ffff}", 26)},
	{std::string("(\077i)[[:lower:]]", 15), std::string("cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}", 36)},
	{std::string("(\077i)[a-z]", 9), std::string("cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}", 36)},
	{std::string("(\077i)[^[:lower:]]", 16), std::string("cc{0x0-0x40 0x5b-0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}", 62)},
	{std::string("(\077i)[[:^lower:]]", 16), std::string("cc{0x0-0x40 0x5b-0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}", 62)},
	{std::string("\134d", 2), std::string("cc{0x30-0x39}", 13)},
	{std::string("\134D", 2), std::string("cc{0x0-0x2f 0x3a-0x10ffff}", 26)},
	{std::string("\134s", 2), std::string("cc{0x9-0xa 0xc-0xd 0x20}", 24)},
	{std::string("\134S", 2), std::string("cc{0x0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}", 38)},
	{std::string("\134w", 2), std::string("cc{0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a}", 38)},
	{std::string("\134W", 2), std::string("cc{0x0-0x2f 0x3a-0x40 0x5b-0x5e 0x60 0x7b-0x10ffff}", 51)},
	{std::string("(\077i)\134w", 6), std::string("cc{0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a 0x17f 0x212a}", 51)},
	{std::string("(\077i)\134W", 6), std::string("cc{0x0-0x2f 0x3a-0x40 0x5b-0x5e 0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}", 77)},
	{std::string("[^\134\134]", 5), std::string("cc{0x0-0x5b 0x5d-0x10ffff}", 26)},
	//	{ `\C`, `byte{}` },  // probably never

	// Unicode, negatives, and a double negative.
	{std::string("\134p{Braille}", 11), std::string("cc{0x2800-0x28ff}", 17)},
	{std::string("\134P{Braille}", 11), std::string("cc{0x0-0x27ff 0x2900-0x10ffff}", 30)},
	{std::string("\134p{^Braille}", 12), std::string("cc{0x0-0x27ff 0x2900-0x10ffff}", 30)},
	{std::string("\134P{^Braille}", 12), std::string("cc{0x2800-0x28ff}", 17)},
	{std::string("\134pZ", 3), std::string("cc{0x20 0xa0 0x1680 0x2000-0x200a 0x2028-0x2029 0x202f 0x205f 0x3000}", 69)},
	{std::string("[\134p{Braille}]", 13), std::string("cc{0x2800-0x28ff}", 17)},
	{std::string("[\134P{Braille}]", 13), std::string("cc{0x0-0x27ff 0x2900-0x10ffff}", 30)},
	{std::string("[\134p{^Braille}]", 14), std::string("cc{0x0-0x27ff 0x2900-0x10ffff}", 30)},
	{std::string("[\134P{^Braille}]", 14), std::string("cc{0x2800-0x28ff}", 17)},
	{std::string("[\134pZ]", 5), std::string("cc{0x20 0xa0 0x1680 0x2000-0x200a 0x2028-0x2029 0x202f 0x205f 0x3000}", 69)},
	{std::string("\134p{Lu}", 6), mkCharClass(unicode::IsUpper)},
	{std::string("\134p{Uppercase_Letter}", 20), mkCharClass(unicode::IsUpper)},
	{std::string("\134p{upper case-let ter}", 22), mkCharClass(unicode::IsUpper)},
	{std::string("\134p{__upper case-let ter}", 24), mkCharClass(unicode::IsUpper)},
	{std::string("\134p{Canadian_Aboriginal}", 23), mkCharClass([](rune r) { return unicode::Is(unicode::Canadian_Aboriginal, r); })},
	{std::string("[\134p{Lu}]", 8), mkCharClass(unicode::IsUpper)},
	{std::string("(\077i)[\134p{Lu}]", 12), mkCharClass(isUpperFold)},
	{std::string("\134p{Any}", 7), std::string("dot{}", 5)},
	{std::string("\134p{^Any}", 8), std::string("cc{}", 4)},
	{std::string("(\077i)\134p{ascii}", 13), std::string("cc{0x0-0x7f 0x17f 0x212a}", 25)},
	{std::string("\134p{Assigned}", 12), mkCharClass([](rune r) { return !unicode::Is(unicode::Cn, r); })},
	{std::string("\134p{^Assigned}", 13), mkCharClass([](rune r) { return unicode::Is(unicode::Cn, r); })},

	// Hex, octal.
	{std::string("[\134012-\134234]\134141", 15), std::string("cat{cc{0xa-0x9c}lit{a}}", 23)},
	{std::string("[\134x{41}-\134x7a]\134x61", 17), std::string("cat{cc{0x41-0x7a}lit{a}}", 24)},

	// More interesting regular expressions.
	{std::string("a{,2}", 5), std::string("str{a{,2}}", 10)},
	{std::string("\134.\134^\134$\134\134", 8), std::string("str{.^$\134}", 9)},
	{std::string("[a-zABC]", 8), std::string("cc{0x41-0x43 0x61-0x7a}", 23)},
	{std::string("[^a]", 4), std::string("cc{0x0-0x60 0x62-0x10ffff}", 26)},
	{std::string("[\316\261-\316\265\342\230\272]", 10), std::string("cc{0x3b1-0x3b5 0x263a}", 22)}, // utf-8
	{std::string("a*{", 3), std::string("cat{star{lit{a}}lit{{}}", 23)},

	// Test precedences
	{std::string("(\077:ab)*", 7), std::string("star{str{ab}}", 13)},
	{std::string("(ab)*", 5), std::string("star{cap{str{ab}}}", 18)},
	{std::string("ab|cd", 5), std::string("alt{str{ab}str{cd}}", 19)},
	{std::string("a(b|c)d", 7), std::string("cat{lit{a}cap{cc{0x62-0x63}}lit{d}}", 35)},

	// Test flattening.
	{std::string("(\077:a)", 5), std::string("lit{a}", 6)},
	{std::string("(\077:ab)(\077:cd)", 12), std::string("str{abcd}", 9)},
	{std::string("(\077:a+b+)(\077:c+d+)", 16), std::string("cat{plus{lit{a}}plus{lit{b}}plus{lit{c}}plus{lit{d}}}", 53)},
	{std::string("(\077:a+|b+)|(\077:c+|d+)", 19), std::string("alt{plus{lit{a}}plus{lit{b}}plus{lit{c}}plus{lit{d}}}", 53)},
	{std::string("(\077:a|b)|(\077:c|d)", 15), std::string("cc{0x61-0x64}", 13)},
	{std::string("a|.", 3), std::string("dot{}", 5)},
	{std::string(".|a", 3), std::string("dot{}", 5)},
	{std::string("(\077:[abc]|A|Z|hello|world)", 25), std::string("alt{cc{0x41 0x5a 0x61-0x63}str{hello}str{world}}", 48)},
	{std::string("(\077:[abc]|A|Z)", 13), std::string("cc{0x41 0x5a 0x61-0x63}", 23)},

	// Test Perl quoted literals
	{std::string("\134Q+|*\077{[\134E", 10), std::string("str{+|*\077{[}", 11)},
	{std::string("\134Q+\134E+", 6), std::string("plus{lit{+}}", 12)},
	{std::string("\134Qab\134E+", 7), std::string("cat{lit{a}plus{lit{b}}}", 23)},
	{std::string("\134Q\134\134E", 5), std::string("lit{\134}", 6)},
	{std::string("\134Q\134\134\134E", 6), std::string("str{\134\134}", 7)},

	// Test Perl \A and \z
	{std::string("(\077m)^", 5), std::string("bol{}", 5)},
	{std::string("(\077m)$", 5), std::string("eol{}", 5)},
	{std::string("(\077-m)^", 6), std::string("bot{}", 5)},
	{std::string("(\077-m)$", 6), std::string("eot{}", 5)},
	{std::string("(\077m)\134A", 6), std::string("bot{}", 5)},
	{std::string("(\077m)\134z", 6), std::string("eot{\134z}", 7)},
	{std::string("(\077-m)\134A", 7), std::string("bot{}", 5)},
	{std::string("(\077-m)\134z", 7), std::string("eot{\134z}", 7)},

	// Test named captures
	{std::string("(\077P<name>a)", 11), std::string("cap{name:lit{a}}", 16)},
	{std::string("(\077<name>a)", 10), std::string("cap{name:lit{a}}", 16)},

	// Case-folded literals
	{std::string("[Aa]", 4), std::string("litfold{A}", 10)},
	{std::string("[\134x{100}\134x{101}]", 16), std::string("litfold{\304\200}", 11)},
	{std::string("[\316\224\316\264]", 6), std::string("litfold{\316\224}", 11)},

	// Strings
	{std::string("abcde", 5), std::string("str{abcde}", 10)},
	{std::string("[Aa][Bb]cd", 10), std::string("cat{strfold{AB}str{cd}}", 23)},

	// Factoring.
	{std::string("abc|abd|aef|bcx|bcy", 19), std::string("alt{cat{lit{a}alt{cat{lit{b}cc{0x63-0x64}}str{ef}}}cat{str{bc}cc{0x78-0x79}}}", 77)},
	{std::string("ax+y|ax+z|ay+w", 14), std::string("cat{lit{a}alt{cat{plus{lit{x}}lit{y}}cat{plus{lit{x}}lit{z}}cat{plus{lit{y}}lit{w}}}}", 85)},

	// Bug fixes.
	{std::string("(\077:.)", 5), std::string("dot{}", 5)},
	{std::string("(\077:x|(\077:xa))", 12), std::string("cat{lit{x}alt{emp{}lit{a}}}", 27)},
	{std::string("(\077:.|(\077:.a))", 12), std::string("cat{dot{}alt{emp{}lit{a}}}", 26)},
	{std::string("(\077:A(\077:A|a))", 12), std::string("cat{lit{A}litfold{A}}", 21)},
	{std::string("(\077:A|a)", 7), std::string("litfold{A}", 10)},
	{std::string("A|(\077:A|a)", 9), std::string("litfold{A}", 10)},
	{std::string("(\077s).", 5), std::string("dot{}", 5)},
	{std::string("(\077-s).", 6), std::string("dnl{}", 5)},
	{std::string("(\077:(\077:^).)", 10), std::string("cat{bol{}dot{}}", 15)},
	{std::string("(\077-s)(\077:(\077:^).)", 15), std::string("cat{bol{}dnl{}}", 15)},
	{std::string("[\134s\134S]a", 7), std::string("cat{cc{0x0-0x10ffff}lit{a}}", 27)},

	// RE2 prefix_tests
	{std::string("abc|abd", 7), std::string("cat{str{ab}cc{0x63-0x64}}", 25)},
	{std::string("a(\077:b)c|abd", 11), std::string("cat{str{ab}cc{0x63-0x64}}", 25)},
	{std::string("abc|abd|aef|bcx|bcy", 19),
		std::string("alt{cat{lit{a}alt{cat{lit{b}cc{0x63-0x64}}str{ef}}}", 51) +
			std::string("cat{str{bc}cc{0x78-0x79}}}", 26)},
	{std::string("abc|x|abd", 9), std::string("alt{str{abc}lit{x}str{abd}}", 27)},
	{std::string("(\077i)abc|ABD", 11), std::string("cat{strfold{AB}cc{0x43-0x44 0x63-0x64}}", 39)},
	{std::string("[ab]c|[ab]d", 11), std::string("cat{cc{0x61-0x62}cc{0x63-0x64}}", 31)},
	{std::string(".c|.d", 5), std::string("cat{dot{}cc{0x63-0x64}}", 23)},
	{std::string("x{2}|x{2}[0-9]", 14),
		std::string("cat{rep{2,2 lit{x}}alt{emp{}cc{0x30-0x39}}}", 43)},
	{std::string("x{2}y|x{2}[0-9]y", 16),
		std::string("cat{rep{2,2 lit{x}}alt{lit{y}cat{cc{0x30-0x39}lit{y}}}}", 55)},
	{std::string("a.*\077c|a.*\077b", 11),
		std::string("cat{lit{a}alt{cat{nstar{dot{}}lit{c}}cat{nstar{dot{}}lit{b}}}}", 62)},

	// Valid repetitions.
	{std::string("((((((((((x{2}){2}){2}){2}){2}){2}){2}){2}){2}))", 48), std::string("", 0)},
	{std::string("((((((((((x{1}){2}){2}){2}){2}){2}){2}){2}){2}){2})", 51), std::string("", 0)},

	// Valid nesting.
	{repeat(std::string("(", 1), 999) + repeat(std::string(")", 1), 999), std::string("", 0)},
	{repeat(std::string("(\077:", 3), 999) + repeat(std::string(")*", 2), 999), std::string("", 0)},
	{std::string("(", 1) + repeat(std::string("|", 1), 12345) + std::string(")", 1), std::string("", 0)}, // not nested at all
};
    return tests;
}

constexpr Flags testFlags = MatchNL | PerlX | UnicodeGroups;

// Test Parse -> Dump.
void testParseDump(const std::vector<parseTest>& tests, Flags flags) {
    for (const auto& tt : tests) {
        auto [re, err] = Parse(tt.Regexp, flags);
        if (err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(tt.Regexp.substr(0, 100)) << "): " << err;
            continue;
        }
        if (tt.Dump.empty()) continue;  // It parsed. That's all we care about.
        EXPECT_EQ(dump(*re), tt.Dump) << "Parse(" << strconv::Quote(tt.Regexp) << ").Dump()";
    }
}

TEST(GoRegexpSyntax, ParseSimple) { testParseDump(parseTests(), testFlags); }

TEST(GoRegexpSyntax, ParseFoldCase) {
    testParseDump({
	{std::string("AbCdE", 5), std::string("strfold{ABCDE}", 14)},
	{std::string("[Aa]", 4), std::string("litfold{A}", 10)},
	{std::string("a", 1), std::string("litfold{A}", 10)},

	// 0x17F is an old English long s (looks like an f) and folds to s.
	// 0x212A is the Kelvin symbol and folds to k.
	{std::string("A[F-g]", 6), std::string("cat{litfold{A}cc{0x41-0x7a 0x17f 0x212a}}", 41)}, // [Aa][A-z...]
	{std::string("[[:upper:]]", 11), std::string("cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}", 36)},
	{std::string("[[:lower:]]", 11), std::string("cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}", 36)},
}, FoldCase);
}

TEST(GoRegexpSyntax, ParseLiteral) {
    testParseDump({
	{std::string("(|)^$.[*+\077]{5,10},\134", 19), std::string("str{(|)^$.[*+\077]{5,10},\134}", 24)},
}, Literal);
}

TEST(GoRegexpSyntax, ParseMatchNL) {
    testParseDump({
	{std::string(".", 1), std::string("dot{}", 5)},
	{std::string("\012", 1), std::string("lit{\012}", 6)},
	{std::string("[^a]", 4), std::string("cc{0x0-0x60 0x62-0x10ffff}", 26)},
	{std::string("[a\134n]", 5), std::string("cc{0xa 0x61}", 12)},
}, MatchNL);
}

TEST(GoRegexpSyntax, ParseNoMatchNL) {
    testParseDump({
	{std::string(".", 1), std::string("dnl{}", 5)},
	{std::string("\012", 1), std::string("lit{\012}", 6)},
	{std::string("[^a]", 4), std::string("cc{0x0-0x9 0xb-0x60 0x62-0x10ffff}", 34)},
	{std::string("[a\134n]", 5), std::string("cc{0xa 0x61}", 12)},
}, 0);
}

TEST(GoRegexpSyntax, FoldConstants) {
    rune last = -1, first = -1;
    for (rune i = 0; i <= unicode::MaxRune; i++) {
        if (unicode::SimpleFold(i) == i) continue;
        if (first == -1) first = i;
        last = i;
    }
    // minFold and maxFold of parse.go.
    EXPECT_EQ(first, 0x0041);
    EXPECT_EQ(last, 0x1e943);
}

TEST(GoRegexpSyntax, ParseInvalidRegexps) {
    std::vector<std::string> invalidRegexps = {
	std::string("(", 1),
	std::string(")", 1),
	std::string("(a", 2),
	std::string("a)", 2),
	std::string("(a))", 4),
	std::string("(a|b|", 5),
	std::string("a|b|)", 5),
	std::string("(a|b|))", 7),
	std::string("(a|b", 4),
	std::string("a|b)", 4),
	std::string("(a|b))", 6),
	std::string("[a-z", 4),
	std::string("([a-z)", 6),
	std::string("[a-z)", 5),
	std::string("([a-z]))", 8),
	std::string("x{1001}", 7),
	std::string("x{9876543210}", 13),
	std::string("x{2,1}", 6),
	std::string("x{1,9876543210}", 15),
	std::string("\377", 1), // Invalid UTF-8
	std::string("[\377]", 3),
	std::string("[\134\377]", 4),
	std::string("\134\377", 2),
	std::string("(\077P<name>a", 10),
	std::string("(\077P<name>", 9),
	std::string("(\077P<name", 8),
	std::string("(\077P<x y>a)", 10),
	std::string("(\077P<>a)", 7),
	std::string("(\077<name>a", 9),
	std::string("(\077<name>", 8),
	std::string("(\077<name", 7),
	std::string("(\077<x y>a)", 9),
	std::string("(\077<>a)", 6),
	std::string("[a-Z]", 5),
	std::string("(\077i)[a-Z]", 9),
	std::string("\134Q\134E*", 5),
	std::string("a{100000}", 9),  // too much repetition
	std::string("a{100000,}", 10), // too much repetition
	std::string("((((((((((x{2}){2}){2}){2}){2}){2}){2}){2}){2}){2})", 51),    // too much repetition
	repeat(std::string("(", 1), 1000) + repeat(std::string(")", 1), 1000),    // too deep
	repeat(std::string("(\077:", 3), 1000) + repeat(std::string(")*", 2), 1000), // too deep
	std::string("(", 1) + repeat(std::string("(xx\077)", 5), 1000) + std::string("){1000}", 7),          // too long
	repeat(std::string("(xx\077){1000}", 11), 1000),                      // too long
	repeat(std::string("\134pL", 3), 27000),                             // too many runes
};
    std::vector<std::string> onlyPerl = {
	std::string("[a-b-c]", 7),
	std::string("\134Qabc\134E", 7),
	std::string("\134Q*+\077{[\134E", 9),
	std::string("\134Q\134\134E", 5),
	std::string("\134Q\134\134\134E", 6),
	std::string("\134Q\134\134\134\134E", 7),
	std::string("\134Q\134\134\134\134\134E", 8),
	std::string("(\077:a)", 5),
	std::string("(\077P<name>a)", 11),
};
    std::vector<std::string> onlyPOSIX = {
	std::string("a++", 3),
	std::string("a**", 3),
	std::string("a\077*", 3),
	std::string("a+*", 3),
	std::string("a{1}*", 5),
	std::string(".{1}{2}.{3}", 11),
};
    for (const auto& regexp : invalidRegexps) {
        if (auto [re, err] = Parse(regexp, Perl); !err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(regexp.substr(0, 100)) << ", Perl) = " << dump(*re) << ", should have failed";
        }
        if (auto [re, err] = Parse(regexp, POSIX); !err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(regexp.substr(0, 100)) << ", POSIX) = " << dump(*re) << ", should have failed";
        }
    }
    for (const auto& regexp : onlyPerl) {
        if (auto [re, err] = Parse(regexp, Perl); err) ADD_FAILURE() << "Parse(" << strconv::Quote(regexp) << ", Perl): " << err;
        if (auto [re, err] = Parse(regexp, POSIX); !err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(regexp) << ", POSIX) = " << dump(*re) << ", should have failed";
        }
    }
    for (const auto& regexp : onlyPOSIX) {
        if (auto [re, err] = Parse(regexp, Perl); !err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(regexp) << ", Perl) = " << dump(*re) << ", should have failed";
        }
        if (auto [re, err] = Parse(regexp, POSIX); err) ADD_FAILURE() << "Parse(" << strconv::Quote(regexp) << ", POSIX): " << err;
    }
}

TEST(GoRegexpSyntax, ToStringEquivalentParse) {
    for (const auto& tt : parseTests()) {
        auto [re, err] = Parse(tt.Regexp, testFlags);
        if (err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(tt.Regexp.substr(0, 100)) << "): " << err;
            continue;
        }
        if (tt.Dump.empty()) continue;  // It parsed. That's all we care about.
        std::string d = dump(*re);
        if (d != tt.Dump) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(tt.Regexp) << ").Dump() = " << d << " want " << tt.Dump;
            continue;
        }
        std::string s = re->String().str();
        if (s != tt.Regexp) {
            // If ToString didn't return the original regexp, it must have
            // found one with fewer parens. Unfortunately we can't check the
            // length here, because ToString produces "\\{" for a literal
            // brace, but "{" is a shorter equivalent in some contexts.
            auto [nre, nerr] = Parse(s, testFlags);
            if (nerr) {
                ADD_FAILURE() << "Parse(" << strconv::Quote(tt.Regexp) << ".String() = " << strconv::Quote(s) << "): " << nerr;
                continue;
            }
            std::string nd = dump(*nre);
            EXPECT_EQ(d, nd) << "Parse(" << strconv::Quote(tt.Regexp) << ") -> " << strconv::Quote(s);
            EXPECT_EQ(s, nre->String().str()) << "Parse(" << strconv::Quote(tt.Regexp) << ") -> " << strconv::Quote(s);
        }
    }
}

TEST(GoRegexpSyntax, String) {
    struct test {
        std::string re, out;
    };
    std::vector<test> stringTests = {
	{std::string("x(\077i:ab*c|d\077e)1", 15), std::string("x(\077i:AB*C|D\077E)1", 15)},
	{std::string("x(\077i:ab*cd\077e)1", 14), std::string("x(\077i:AB*CD\077E)1", 14)},
	{std::string("0(\077i:ab*c|d\077e)1", 15), std::string("(\077i:0(\077:AB*C|D\077E)1)", 19)},
	{std::string("0(\077i:ab*cd\077e)1", 14), std::string("(\077i:0AB*CD\077E1)", 14)},
	{std::string("x(\077i:ab*c|d\077e)", 14), std::string("x(\077i:AB*C|D\077E)", 14)},
	{std::string("x(\077i:ab*cd\077e)", 13), std::string("x(\077i:AB*CD\077E)", 13)},
	{std::string("0(\077i:ab*c|d\077e)", 14), std::string("(\077i:0(\077:AB*C|D\077E))", 18)},
	{std::string("0(\077i:ab*cd\077e)", 13), std::string("(\077i:0AB*CD\077E)", 13)},
	{std::string("(\077i:ab*c|d\077e)1", 14), std::string("(\077i:(\077:AB*C|D\077E)1)", 18)},
	{std::string("(\077i:ab*cd\077e)1", 13), std::string("(\077i:AB*CD\077E1)", 13)},
	{std::string("(\077i:ab)[123](\077i:cd)", 19), std::string("(\077i:AB[1-3]CD)", 14)},
	{std::string("(\077i:ab*c|d\077e)", 13), std::string("(\077i:AB*C|D\077E)", 13)},
	{std::string("[Aa][Bb]", 8), std::string("(\077i:AB)", 7)},
	{std::string("[Aa][Bb]*[Cc]", 13), std::string("(\077i:AB*C)", 9)},
	{std::string("A(\077:[Bb][Cc]|[Dd])[Zz]", 22), std::string("A(\077i:(\077:BC|D)Z)", 15)},
	{std::string("[Aa](\077:[Bb][Cc]|[Dd])Z", 22), std::string("(\077i:A(\077:BC|D))Z", 15)},
};
    for (const auto& tt : stringTests) {
        auto [re, err] = Parse(tt.re, Perl);
        if (err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(tt.re) << "): " << err;
            continue;
        }
        EXPECT_EQ(re->String(), tt.out) << "Parse(" << strconv::Quote(tt.re) << ").String()";
    }
}

// ---------------------------------------------------------------------------
// simplify_test.go

TEST(GoRegexpSyntax, Simplify) {
    struct test {
        std::string Regexp, Simple;
    };
    std::vector<test> simplifyTests = {
	// Already-simple constructs
	{std::string("a", 1), std::string("a", 1)},
	{std::string("ab", 2), std::string("ab", 2)},
	{std::string("a|b", 3), std::string("[ab]", 4)},
	{std::string("ab|cd", 5), std::string("ab|cd", 5)},
	{std::string("(ab)*", 5), std::string("(ab)*", 5)},
	{std::string("(ab)+", 5), std::string("(ab)+", 5)},
	{std::string("(ab)\077", 5), std::string("(ab)\077", 5)},
	{std::string(".", 1), std::string("(\077s:.)", 6)},
	{std::string("^", 1), std::string("(\077m:^)", 6)},
	{std::string("$", 1), std::string("(\077m:$)", 6)},
	{std::string("[ac]", 4), std::string("[ac]", 4)},
	{std::string("[^ac]", 5), std::string("[^ac]", 5)},

	// Posix character classes
	{std::string("[[:alnum:]]", 11), std::string("[0-9A-Za-z]", 11)},
	{std::string("[[:alpha:]]", 11), std::string("[A-Za-z]", 8)},
	{std::string("[[:blank:]]", 11), std::string("[\134t ]", 5)},
	{std::string("[[:cntrl:]]", 11), std::string("[\134x00-\134x1f\134x7f]", 15)},
	{std::string("[[:digit:]]", 11), std::string("[0-9]", 5)},
	{std::string("[[:graph:]]", 11), std::string("[!-~]", 5)},
	{std::string("[[:lower:]]", 11), std::string("[a-z]", 5)},
	{std::string("[[:print:]]", 11), std::string("[ -~]", 5)},
	{std::string("[[:punct:]]", 11), std::string("[!-/:-@\134[-`\134{-~]", 16)},
	{std::string("[[:space:]]", 11), std::string("[\134t-\134r ]", 8)},
	{std::string("[[:upper:]]", 11), std::string("[A-Z]", 5)},
	{std::string("[[:xdigit:]]", 12), std::string("[0-9A-Fa-f]", 11)},

	// Perl character classes
	{std::string("\134d", 2), std::string("[0-9]", 5)},
	{std::string("\134s", 2), std::string("[\134t\134n\134f\134r ]", 11)},
	{std::string("\134w", 2), std::string("[0-9A-Z_a-z]", 12)},
	{std::string("\134D", 2), std::string("[^0-9]", 6)},
	{std::string("\134S", 2), std::string("[^\134t\134n\134f\134r ]", 12)},
	{std::string("\134W", 2), std::string("[^0-9A-Z_a-z]", 13)},
	{std::string("[\134d]", 4), std::string("[0-9]", 5)},
	{std::string("[\134s]", 4), std::string("[\134t\134n\134f\134r ]", 11)},
	{std::string("[\134w]", 4), std::string("[0-9A-Z_a-z]", 12)},
	{std::string("[\134D]", 4), std::string("[^0-9]", 6)},
	{std::string("[\134S]", 4), std::string("[^\134t\134n\134f\134r ]", 12)},
	{std::string("[\134W]", 4), std::string("[^0-9A-Z_a-z]", 13)},

	// Posix repetitions
	{std::string("a{1}", 4), std::string("a", 1)},
	{std::string("a{2}", 4), std::string("aa", 2)},
	{std::string("a{5}", 4), std::string("aaaaa", 5)},
	{std::string("a{0,1}", 6), std::string("a\077", 2)},
	// The next three are illegible because Simplify inserts (?:)
	// parens instead of () parens to avoid creating extra
	// captured subexpressions. The comments show a version with fewer parens.
	{std::string("(a){0,2}", 8), std::string("(\077:(a)(a)\077)\077", 12)},                       //       (aa?)?
	{std::string("(a){0,4}", 8), std::string("(\077:(a)(\077:(a)(\077:(a)(a)\077)\077)\077)\077", 28)},       //   (a(a(aa?)?)?)?
	{std::string("(a){2,6}", 8), std::string("(a)(a)(\077:(a)(\077:(a)(\077:(a)(a)\077)\077)\077)\077", 34)}, // aa(a(a(aa?)?)?)?
	{std::string("a{0,2}", 6), std::string("(\077:aa\077)\077", 8)},                             //       (aa?)?
	{std::string("a{0,4}", 6), std::string("(\077:a(\077:a(\077:aa\077)\077)\077)\077", 20)},                 //   (a(a(aa?)?)?)?
	{std::string("a{2,6}", 6), std::string("aa(\077:a(\077:a(\077:aa\077)\077)\077)\077", 22)},               // aa(a(a(aa?)?)?)?
	{std::string("a{0,}", 5), std::string("a*", 2)},
	{std::string("a{1,}", 5), std::string("a+", 2)},
	{std::string("a{2,}", 5), std::string("aa+", 3)},
	{std::string("a{5,}", 5), std::string("aaaaa+", 6)},

	// Test that operators simplify their arguments.
	{std::string("(\077:a{1,}){1,}", 13), std::string("a+", 2)},
	{std::string("(a{1,}b{1,})", 12), std::string("(a+b+)", 6)},
	{std::string("a{1,}|b{1,}", 11), std::string("a+|b+", 5)},
	{std::string("(\077:a{1,})*", 10), std::string("(\077:a+)*", 7)},
	{std::string("(\077:a{1,})+", 10), std::string("a+", 2)},
	{std::string("(\077:a{1,})\077", 10), std::string("(\077:a+)\077", 7)},
	{std::string("", 0), std::string("(\077:)", 4)},
	{std::string("a{0}", 4), std::string("(\077:)", 4)},

	// Character class simplification
	{std::string("[ab]", 4), std::string("[ab]", 4)},
	{std::string("[abc]", 5), std::string("[a-c]", 5)},
	{std::string("[a-za-za-z]", 11), std::string("[a-z]", 5)},
	{std::string("[A-Za-zA-Za-z]", 14), std::string("[A-Za-z]", 8)},
	{std::string("[ABCDEFGH]", 10), std::string("[A-H]", 5)},
	{std::string("[AB-CD-EF-GH]", 13), std::string("[A-H]", 5)},
	{std::string("[W-ZP-XE-R]", 11), std::string("[E-Z]", 5)},
	{std::string("[a-ee-gg-m]", 11), std::string("[a-m]", 5)},
	{std::string("[a-ea-ha-m]", 11), std::string("[a-m]", 5)},
	{std::string("[a-ma-ha-e]", 11), std::string("[a-m]", 5)},
	{std::string("[a-zA-Z0-9 -~]", 14), std::string("[ -~]", 5)},

	// Empty character classes
	{std::string("[^[:cntrl:][:^cntrl:]]", 22), std::string("[^\134x00-\134x{10FFFF}]", 18)},

	// Full character classes
	{std::string("[[:cntrl:][:^cntrl:]]", 21), std::string("(\077s:.)", 6)},

	// Unicode case folding.
	{std::string("(\077i)A", 5), std::string("(\077i:A)", 6)},
	{std::string("(\077i)a", 5), std::string("(\077i:A)", 6)},
	{std::string("(\077i)[A]", 7), std::string("(\077i:A)", 6)},
	{std::string("(\077i)[a]", 7), std::string("(\077i:A)", 6)},
	{std::string("(\077i)K", 5), std::string("(\077i:K)", 6)},
	{std::string("(\077i)k", 5), std::string("(\077i:K)", 6)},
	{std::string("(\077i)\134x{212a}", 12), std::string("(\077i:K)", 6)},
	{std::string("(\077i)[K]", 7), std::string("[Kk\342\204\252]", 7)},
	{std::string("(\077i)[k]", 7), std::string("[Kk\342\204\252]", 7)},
	{std::string("(\077i)[\134x{212a}]", 14), std::string("[Kk\342\204\252]", 7)},
	{std::string("(\077i)[a-z]", 9), std::string("[A-Za-z\305\277\342\204\252]", 13)},
	{std::string("(\077i)[\134x00-\134x{FFFD}]", 19), std::string("[\134x00-\357\277\275]", 10)},
	{std::string("(\077i)[\134x00-\134x{10FFFF}]", 21), std::string("(\077s:.)", 6)},

	// Empty string as a regular expression.
	// The empty string must be preserved inside parens in order
	// to make submatches work right, so these tests are less
	// interesting than they might otherwise be. String inserts
	// explicit (?:) in place of non-parenthesized empty strings,
	// to make them easier to spot for other parsers.
	{std::string("(a|b|c|)", 8), std::string("([a-c]|(\077:))", 12)},
	{std::string("(a|b|)", 6), std::string("([ab]|(\077:))", 11)},
	{std::string("(|)", 3), std::string("()", 2)},
	{std::string("a()", 3), std::string("a()", 3)},
	{std::string("(()|())", 7), std::string("(()|())", 7)},
	{std::string("(a|)", 4), std::string("(a|(\077:))", 8)},
	{std::string("ab()cd()", 8), std::string("ab()cd()", 8)},
	{std::string("()", 2), std::string("()", 2)},
	{std::string("()*", 3), std::string("()*", 3)},
	{std::string("()+", 3), std::string("()+", 3)},
	{std::string("()\077", 3), std::string("()\077", 3)},
	{std::string("(){0}", 5), std::string("(\077:)", 4)},
	{std::string("(){1}", 5), std::string("()", 2)},
	{std::string("(){1,}", 6), std::string("()+", 3)},
	{std::string("(){0,2}", 7), std::string("(\077:()()\077)\077", 10)},
};
    for (const auto& tt : simplifyTests) {
        auto [re, err] = Parse(tt.Regexp, MatchNL | (Perl & ~OneLine));
        if (err) {
            ADD_FAILURE() << "Parse(" << strconv::Quote(tt.Regexp) << ") = error " << err;
            continue;
        }
        EXPECT_EQ(re->Simplify()->String(), tt.Simple) << "Simplify(" << strconv::Quote(tt.Regexp) << ")";
    }
}

// ---------------------------------------------------------------------------
// prog_test.go

TEST(GoRegexpSyntax, Compile) {
    struct test {
        std::string Regexp, Prog;
    };
    std::vector<test> compileTests = {
	{std::string("a", 1), std::string("  0\011fail\012  1*\011rune1 \042a\042 -> 2\012  2\011match\012", 39)},
	{std::string("[A-M][n-z]", 10), std::string("  0\011fail\012  1*\011rune \042AM\042 -> 2\012  2\011rune \042nz\042 -> 3\012  3\011match\012", 58)},
	{std::string("", 0), std::string("  0\011fail\012  1*\011nop -> 2\012  2\011match\012", 33)},
	{std::string("a\077", 2), std::string("  0\011fail\012  1\011rune1 \042a\042 -> 3\012  2*\011alt -> 1, 3\012  3\011match\012", 55)},
	{std::string("a\077\077", 3), std::string("  0\011fail\012  1\011rune1 \042a\042 -> 3\012  2*\011alt -> 3, 1\012  3\011match\012", 55)},
	{std::string("a+", 2), std::string("  0\011fail\012  1*\011rune1 \042a\042 -> 2\012  2\011alt -> 1, 3\012  3\011match\012", 55)},
	{std::string("a+\077", 3), std::string("  0\011fail\012  1*\011rune1 \042a\042 -> 2\012  2\011alt -> 3, 1\012  3\011match\012", 55)},
	{std::string("a*", 2), std::string("  0\011fail\012  1\011rune1 \042a\042 -> 2\012  2*\011alt -> 1, 3\012  3\011match\012", 55)},
	{std::string("a*\077", 3), std::string("  0\011fail\012  1\011rune1 \042a\042 -> 2\012  2*\011alt -> 3, 1\012  3\011match\012", 55)},
	{std::string("a+b+", 4), std::string("  0\011fail\012  1*\011rune1 \042a\042 -> 2\012  2\011alt -> 1, 3\012  3\011rune1 \042b\042 -> 4\012  4\011alt -> 3, 5\012  5\011match\012", 90)},
	{std::string("(a+)(b+)", 8), std::string("  0\011fail\012  1*\011cap 2 -> 2\012  2\011rune1 \042a\042 -> 3\012  3\011alt -> 2, 4\012  4\011cap 3 -> 5\012  5\011cap 4 -> 6\012  6\011rune1 \042b\042 -> 7\012  7\011alt -> 6, 8\012  8\011cap 5 -> 9\012  9\011match\012", 150)},
	{std::string("a+|b+", 5), std::string("  0\011fail\012  1\011rune1 \042a\042 -> 2\012  2\011alt -> 1, 6\012  3\011rune1 \042b\042 -> 4\012  4\011alt -> 3, 6\012  5*\011alt -> 1, 3\012  6\011match\012", 106)},
	{std::string("A[Aa]", 5), std::string("  0\011fail\012  1*\011rune1 \042A\042 -> 2\012  2\011rune \042A\042/i -> 3\012  3\011match\012", 59)},
	{std::string("(\077:(\077:^).)", 10), std::string("  0\011fail\012  1*\011empty 4 -> 2\012  2\011anynotnl -> 3\012  3\011match\012", 55)},
	{std::string("(\077:|a)+", 7), std::string("  0\011fail\012  1\011nop -> 4\012  2\011rune1 \042a\042 -> 4\012  3*\011alt -> 1, 2\012  4\011alt -> 3, 5\012  5\011match\012", 84)},
	{std::string("(\077:|a)*", 7), std::string("  0\011fail\012  1\011nop -> 4\012  2\011rune1 \042a\042 -> 4\012  3\011alt -> 1, 2\012  4\011alt -> 3, 6\012  5*\011alt -> 3, 6\012  6\011match\012", 100)},
};
    for (const auto& tt : compileTests) {
        auto [re, _] = Parse(tt.Regexp, Perl);
        auto [p, _2] = Compile(*re);
        EXPECT_EQ(p->String(), tt.Prog) << "compiled " << strconv::Quote(tt.Regexp);
    }
}

// ---------------------------------------------------------------------------
// onepass_test.go

TEST(GoRegexpSyntax, RunOnePass) {
    // golang.org/issue/11905 (Go also checks that the program is one-pass,
    // which go++ doesn't expose).
    auto [re, err] = regexp::Compile(R"(^a(/b+(#c+)*)*$)");
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(re->MatchString("a/b#c")) << "onepass did not match";
}

}  // namespace
