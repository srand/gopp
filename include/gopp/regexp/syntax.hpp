// The regexp/syntax package: parses regular expressions into syntax trees
// and compiles them into programs, ported from Go 1.27.3. Most programs use
// regexp instead.
//
//     auto [re, err] = syntax::Parse(`a+(b|c)`, syntax::Perl);
//     re = re->Simplify();
//     auto [prog, err2] = syntax::Compile(re);
//
// The syntax is RE2's, as Go's regexp package documents it.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::syntax {

// syntax.ErrorCode: the kind of a parse error; its text.
using ErrorCode = std::string_view;

inline constexpr ErrorCode ErrInternalError = "regexp/syntax: internal error";
inline constexpr ErrorCode ErrInvalidCharClass = "invalid character class";
inline constexpr ErrorCode ErrInvalidCharRange = "invalid character class range";
inline constexpr ErrorCode ErrInvalidEscape = "invalid escape sequence";
inline constexpr ErrorCode ErrInvalidNamedCapture = "invalid named capture";
inline constexpr ErrorCode ErrInvalidPerlOp = "invalid or unsupported Perl syntax";
inline constexpr ErrorCode ErrInvalidRepeatOp = "invalid nested repetition operator";
inline constexpr ErrorCode ErrInvalidRepeatSize = "invalid repeat count";
inline constexpr ErrorCode ErrInvalidUTF8 = "invalid UTF-8";
inline constexpr ErrorCode ErrMissingBracket = "missing closing ]";
inline constexpr ErrorCode ErrMissingParen = "missing closing )";
inline constexpr ErrorCode ErrMissingRepeatArgument = "missing argument to repetition operator";
inline constexpr ErrorCode ErrTrailingBackslash = "trailing backslash at end of expression";
inline constexpr ErrorCode ErrUnexpectedParen = "unexpected )";
inline constexpr ErrorCode ErrNestingDepth = "expression nests too deeply";
inline constexpr ErrorCode ErrLarge = "expression too large";

namespace detail {
// Implements Error() for an error type named Error, which can't declare it.
template <typename E>
struct namedError : errorInterface {
    string Error() const override { return static_cast<const E&>(*this).message(); }
};
}  // namespace detail

// syntax.Error: a parse error: its code and the expression it's about.
struct Error : detail::namedError<Error> {
    Error(ErrorCode code, string expr) : Code(code), Expr(std::move(expr)) {}
    // "error parsing regexp: <code>: `<expr>`"
    string message() const;
    ErrorCode Code;
    string Expr;
};

// syntax.Flags: parser flags.
using Flags = uint16;
inline constexpr Flags FoldCase = 1 << 0;       // case-insensitive match
inline constexpr Flags Literal = 1 << 1;        // treat pattern as literal string
inline constexpr Flags ClassNL = 1 << 2;        // allow character classes like [^a-z] and [[:space:]] to match newline
inline constexpr Flags DotNL = 1 << 3;          // allow . to match newline
inline constexpr Flags OneLine = 1 << 4;        // treat ^ and $ as only matching at beginning and end of text
inline constexpr Flags NonGreedy = 1 << 5;      // make repetition operators default to non-greedy
inline constexpr Flags PerlX = 1 << 6;          // allow Perl extensions
inline constexpr Flags UnicodeGroups = 1 << 7;  // allow \p{Han}, \P{Han} for Unicode group and negation
inline constexpr Flags WasDollar = 1 << 8;      // regexp OpEndText was $, not \z
inline constexpr Flags Simple = 1 << 9;         // regexp contains no counted repetition
inline constexpr Flags MatchNL = ClassNL | DotNL;
inline constexpr Flags Perl = ClassNL | OneLine | PerlX | UnicodeGroups;  // as close to Perl as possible
inline constexpr Flags POSIX = 0;                                         // POSIX syntax

// syntax.Op: the operator of a Regexp node.
enum Op : uint8 {
    OpNoMatch = 1,     // matches no strings
    OpEmptyMatch,      // matches empty string
    OpLiteral,         // matches Runes sequence
    OpCharClass,       // matches Runes interpreted as range pair list
    OpAnyCharNotNL,    // matches any character except newline
    OpAnyChar,         // matches any character
    OpBeginLine,       // matches empty string at beginning of line
    OpEndLine,         // matches empty string at end of line
    OpBeginText,       // matches empty string at beginning of text
    OpEndText,         // matches empty string at end of text
    OpWordBoundary,    // matches word boundary `\b`
    OpNoWordBoundary,  // matches word non-boundary `\B`
    OpCapture,         // capturing subexpression with index Cap, optional name Name
    OpStar,            // matches Sub[0] zero or more times
    OpPlus,            // matches Sub[0] one or more times
    OpQuest,           // matches Sub[0] zero or one times
    OpRepeat,          // matches Sub[0] at least Min times, at most Max (Max == -1 is no limit)
    OpConcat,          // matches concatenation of Subs
    OpAlternate,       // matches alternation of Subs
};

// Op.String(): "NoMatch", "Literal", ...
string String(Op op);

// syntax.Regexp: a node of a syntax tree.
class Regexp final : public object {
public:
    syntax::Op Op{};
    syntax::Flags Flags = 0;
    std::vector<ptr<Regexp>> Sub;  // subexpressions, if any
    std::vector<rune> Rune;        // matched runes, for OpLiteral, OpCharClass
    int Min = 0, Max = 0;          // min, max for OpRepeat
    int Cap = 0;                   // capturing index, for OpCapture
    string Name;                   // capturing name, for OpCapture

    Regexp() = default;
    explicit Regexp(syntax::Op op) : Op(op) {}

    // Whether x and y have identical structure.
    bool Equal(const Regexp* y) const;
    // The regular expression as a string, which parses to an equivalent
    // one.
    string String() const;
    // The largest capture index in the tree.
    int MaxCap() const;
    // The names of the capture groups, by index; "" for unnamed ones and
    // index 0 (the whole match).
    std::vector<string> CapNames() const;
    // An equivalent tree without counted repetitions and with other
    // simplifications: x{1,2} as x(x)?, and so on. It may share subtrees
    // with this one.
    ptr<Regexp> Simplify();

private:
    void capNames(std::vector<string>& names) const;
};

// syntax.Parse: parses s, with flags, into a syntax tree. Errors are
// *Error.
std::pair<ptr<Regexp>, error> Parse(std::string_view s, Flags flags);

// syntax.InstOp: the opcode of an instruction.
enum InstOp : uint8 {
    InstAlt,
    InstAltMatch,
    InstCapture,
    InstEmptyWidth,
    InstMatch,
    InstFail,
    InstNop,
    InstRune,
    InstRune1,
    InstRuneAny,
    InstRuneAnyNotNL,
};

// InstOp.String(): "InstAlt", ...
string String(InstOp op);

// syntax.EmptyOp: the conditions of an InstEmptyWidth, as bits.
using EmptyOp = uint8;
inline constexpr EmptyOp EmptyBeginLine = 1 << 0;
inline constexpr EmptyOp EmptyEndLine = 1 << 1;
inline constexpr EmptyOp EmptyBeginText = 1 << 2;
inline constexpr EmptyOp EmptyEndText = 1 << 3;
inline constexpr EmptyOp EmptyWordBoundary = 1 << 4;
inline constexpr EmptyOp EmptyNoWordBoundary = 1 << 5;

// syntax.EmptyOpContext: the conditions that hold between the runes r1 and
// r2; -1 stands for the start or end of the text.
EmptyOp EmptyOpContext(rune r1, rune r2) noexcept;

// syntax.IsWordChar: whether r is a word character for \b and \B (ASCII
// letters, digits and underscore).
constexpr bool IsWordChar(rune r) noexcept {
    return ('a' <= r && r <= 'z') || ('A' <= r && r <= 'Z') || ('0' <= r && r <= '9') || r == '_';
}

// syntax.Inst: an instruction of a program.
struct Inst {
    InstOp Op{};
    uint32 Out = 0;  // all but InstMatch, InstFail
    uint32 Arg = 0;  // InstAlt, InstAltMatch, InstCapture, InstEmptyWidth
    std::vector<rune> Rune;

    // Whether r matches this rune instruction.
    bool MatchRune(rune r) const noexcept { return MatchRunePos(r) != -1; }
    // The index of the pair of Rune that r is in, or -1.
    int MatchRunePos(rune r) const noexcept;
    // Whether this empty-width instruction matches between before and after.
    bool MatchEmptyWidth(rune before, rune after) const;
    string String() const;
};

// syntax.Prog: a compiled program.
struct Prog final : object {
    std::vector<syntax::Inst> Inst;
    int Start = 0;   // index of start instruction
    int NumCap = 0;  // number of InstCapture insts in re

    // The program listing.
    string String() const;
    // The literal prefix every match starts with, and whether it is the
    // whole match.
    std::pair<string, bool> Prefix() const;
    // The empty-width conditions that must hold at the start of a match;
    // all bits if it can't match.
    EmptyOp StartCond() const;

    // The instruction at pc after skipping Nop and Capture.
    const syntax::Inst* skipNop(uint32 pc) const;
};

// syntax.Compile: compiles a (simplified) syntax tree into a program.
std::pair<ptr<Prog>, error> Compile(const Regexp& re);

}  // namespace gopp::syntax
