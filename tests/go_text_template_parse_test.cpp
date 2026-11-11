// Ported from Go 1.27.3's text/template/parse: lex_test.go and
// parse_test.go, with Go's test names and tables (the tables are generated
// from the Go sources). Left out: the benchmarks. TestNumberParse decides
// complex inputs by their form where Go asks fmt.Sscan.
#include "text/template/parse/lex.hpp"

#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <complex>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::text::template_::parse;
using namespace std::complex_literals;

namespace {

item mk(itemType typ, std::string_view text) { return item{typ, 0, string(text), 0}; }

struct lex_test {
    std::string name;
    std::string input;
    std::vector<item> items;
};

struct number_test {
    std::string text;
    bool isInt = false;
    bool isUint = false;
    bool isFloat = false;
    bool isComplex = false;
    int64 i64 = 0;
    uint64 u64 = 0;
    double f64 = 0;
    std::complex<double> c128 = 0;
};

constexpr bool noError = true;
constexpr bool hasError = false;

struct parse_test {
    std::string name;
    std::string input;
    bool ok;
    std::string result;  // what the user would see in an error message
};

struct is_empty_test {
    std::string name;
    std::string input;
    bool empty;
};

// Generated from Go 1.27.3 text/template/parse lex_test.go and parse_test.go.
// clang-format off
namespace lex_tables {
const item tDot = mk(itemDot, ".");
const item tBlock = mk(itemBlock, "block");
const item tEOF = mk(itemEOF, "");
const item tFor = mk(itemIdentifier, "for");
const item tLeft = mk(itemLeftDelim, "{{");
const item tLpar = mk(itemLeftParen, "(");
const item tPipe = mk(itemPipe, "|");
const item tQuote = mk(itemString, std::string(R"__("abc \n\t\" ")__"));
const item tRange = mk(itemRange, "range");
const item tRight = mk(itemRightDelim, "}}");
const item tRpar = mk(itemRightParen, ")");
const item tSpace = mk(itemSpace, " ");
const std::string raw = "`" + std::string(R"__(abc\n\t\" )__") + "`";
const std::string rawNL = "`now is{{\n}}the time`"; // Contains newline inside raw quote.
const item tRawQuote = mk(itemRawString, raw);
const item tRawQuoteNL = mk(itemRawString, rawNL);

const std::vector<lex_test> lexTests = {
	{"empty", "", {tEOF}},
	{"spaces", " \t\n", {mk(itemText, " \t\n"), tEOF}},
	{"text", std::string(R"__(now is the time)__"), {mk(itemText, "now is the time"), tEOF}},
	{"text with comment", "hello-{{/* this is a comment */}}-world", {
		mk(itemText, "hello-"),
		mk(itemComment, "/* this is a comment */"),
		mk(itemText, "-world"),
		tEOF,
	}},
	{"punctuation", "{{,@% }}", {
		tLeft,
		mk(itemChar, ","),
		mk(itemChar, "@"),
		mk(itemChar, "%"),
		tSpace,
		tRight,
		tEOF,
	}},
	{"parens", "{{((3))}}", {
		tLeft,
		tLpar,
		tLpar,
		mk(itemNumber, "3"),
		tRpar,
		tRpar,
		tRight,
		tEOF,
	}},
	{"empty action", std::string(R"__({{}})__"), {tLeft, tRight, tEOF}},
	{"for", std::string(R"__({{for}})__"), {tLeft, tFor, tRight, tEOF}},
	{"block", std::string(R"__({{block "foo" .}})__"), {
		tLeft, tBlock, tSpace, mk(itemString, std::string(R"__("foo")__")), tSpace, tDot, tRight, tEOF,
	}},
	{"quote", std::string(R"__({{"abc \n\t\" "}})__"), {tLeft, tQuote, tRight, tEOF}},
	{"raw quote", "{{" + raw + "}}", {tLeft, tRawQuote, tRight, tEOF}},
	{"raw quote with newline", "{{" + rawNL + "}}", {tLeft, tRawQuoteNL, tRight, tEOF}},
	{"numbers", "{{1 02 0x14 0X14 -7.2i 1e3 1E3 +1.2e-4 4.2i 1+2i 1_2 0x1.e_fp4 0X1.E_FP4}}", {
		tLeft,
		mk(itemNumber, "1"),
		tSpace,
		mk(itemNumber, "02"),
		tSpace,
		mk(itemNumber, "0x14"),
		tSpace,
		mk(itemNumber, "0X14"),
		tSpace,
		mk(itemNumber, "-7.2i"),
		tSpace,
		mk(itemNumber, "1e3"),
		tSpace,
		mk(itemNumber, "1E3"),
		tSpace,
		mk(itemNumber, "+1.2e-4"),
		tSpace,
		mk(itemNumber, "4.2i"),
		tSpace,
		mk(itemComplex, "1+2i"),
		tSpace,
		mk(itemNumber, "1_2"),
		tSpace,
		mk(itemNumber, "0x1.e_fp4"),
		tSpace,
		mk(itemNumber, "0X1.E_FP4"),
		tRight,
		tEOF,
	}},
	{"characters", std::string(R"__({{'a' '\n' '\'' '\\' '\u00FF' '\xFF' '本'}})__"), {
		tLeft,
		mk(itemCharConstant, std::string(R"__('a')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('\n')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('\'')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('\\')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('\u00FF')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('\xFF')__")),
		tSpace,
		mk(itemCharConstant, std::string(R"__('本')__")),
		tRight,
		tEOF,
	}},
	{"bools", "{{true false}}", {
		tLeft,
		mk(itemBool, "true"),
		tSpace,
		mk(itemBool, "false"),
		tRight,
		tEOF,
	}},
	{"dot", "{{.}}", {
		tLeft,
		tDot,
		tRight,
		tEOF,
	}},
	{"nil", "{{nil}}", {
		tLeft,
		mk(itemNil, "nil"),
		tRight,
		tEOF,
	}},
	{"dots", "{{.x . .2 .x.y.z}}", {
		tLeft,
		mk(itemField, ".x"),
		tSpace,
		tDot,
		tSpace,
		mk(itemNumber, ".2"),
		tSpace,
		mk(itemField, ".x"),
		mk(itemField, ".y"),
		mk(itemField, ".z"),
		tRight,
		tEOF,
	}},
	{"keywords", "{{range if else end with}}", {
		tLeft,
		mk(itemRange, "range"),
		tSpace,
		mk(itemIf, "if"),
		tSpace,
		mk(itemElse, "else"),
		tSpace,
		mk(itemEnd, "end"),
		tSpace,
		mk(itemWith, "with"),
		tRight,
		tEOF,
	}},
	{"variables", "{{$c := printf $ $hello $23 $ $var.Field .Method}}", {
		tLeft,
		mk(itemVariable, "$c"),
		tSpace,
		mk(itemDeclare, ":="),
		tSpace,
		mk(itemIdentifier, "printf"),
		tSpace,
		mk(itemVariable, "$"),
		tSpace,
		mk(itemVariable, "$hello"),
		tSpace,
		mk(itemVariable, "$23"),
		tSpace,
		mk(itemVariable, "$"),
		tSpace,
		mk(itemVariable, "$var"),
		mk(itemField, ".Field"),
		tSpace,
		mk(itemField, ".Method"),
		tRight,
		tEOF,
	}},
	{"variable invocation", "{{$x 23}}", {
		tLeft,
		mk(itemVariable, "$x"),
		tSpace,
		mk(itemNumber, "23"),
		tRight,
		tEOF,
	}},
	{"pipeline", std::string(R"__(intro {{echo hi 1.2 |noargs|args 1 "hi"}} outro)__"), {
		mk(itemText, "intro "),
		tLeft,
		mk(itemIdentifier, "echo"),
		tSpace,
		mk(itemIdentifier, "hi"),
		tSpace,
		mk(itemNumber, "1.2"),
		tSpace,
		tPipe,
		mk(itemIdentifier, "noargs"),
		tPipe,
		mk(itemIdentifier, "args"),
		tSpace,
		mk(itemNumber, "1"),
		tSpace,
		mk(itemString, std::string(R"__("hi")__")),
		tRight,
		mk(itemText, " outro"),
		tEOF,
	}},
	{"declaration", "{{$v := 3}}", {
		tLeft,
		mk(itemVariable, "$v"),
		tSpace,
		mk(itemDeclare, ":="),
		tSpace,
		mk(itemNumber, "3"),
		tRight,
		tEOF,
	}},
	{"2 declarations", "{{$v , $w := 3}}", {
		tLeft,
		mk(itemVariable, "$v"),
		tSpace,
		mk(itemChar, ","),
		tSpace,
		mk(itemVariable, "$w"),
		tSpace,
		mk(itemDeclare, ":="),
		tSpace,
		mk(itemNumber, "3"),
		tRight,
		tEOF,
	}},
	{"field of parenthesized expression", "{{(.X).Y}}", {
		tLeft,
		tLpar,
		mk(itemField, ".X"),
		tRpar,
		mk(itemField, ".Y"),
		tRight,
		tEOF,
	}},
	{"trimming spaces before and after", "hello- {{- 3 -}} -world", {
		mk(itemText, "hello-"),
		tLeft,
		mk(itemNumber, "3"),
		tRight,
		mk(itemText, "-world"),
		tEOF,
	}},
	{"trimming spaces before and after comment", "hello- {{- /* hello */ -}} -world", {
		mk(itemText, "hello-"),
		mk(itemComment, "/* hello */"),
		mk(itemText, "-world"),
		tEOF,
	}},
	// errors
	{"badchar", "#{{\x01}}", {
		mk(itemText, "#"),
		tLeft,
		mk(itemError, "unrecognized character in action: U+0001"),
	}},
	{"unclosed action", "{{", {
		tLeft,
		mk(itemError, "unclosed action"),
	}},
	{"EOF in action", "{{range", {
		tLeft,
		tRange,
		mk(itemError, "unclosed action"),
	}},
	{"unclosed quote", "{{\"\n\"}}", {
		tLeft,
		mk(itemError, "unterminated quoted string"),
	}},
	{"unclosed raw quote", "{{`xx}}", {
		tLeft,
		mk(itemError, "unterminated raw quoted string"),
	}},
	{"unclosed char constant", "{{'\n}}", {
		tLeft,
		mk(itemError, "unterminated character constant"),
	}},
	{"bad number", "{{3k}}", {
		tLeft,
		mk(itemError, std::string(R"__(bad number syntax: "3k")__")),
	}},
	{"unclosed paren", "{{(3}}", {
		tLeft,
		tLpar,
		mk(itemNumber, "3"),
		mk(itemError, std::string(R"__(unclosed left paren)__")),
	}},
	{"extra right paren", "{{3)}}", {
		tLeft,
		mk(itemNumber, "3"),
		mk(itemError, "unexpected right paren"),
	}},

	// Fixed bugs
	// Many elements in an action blew the lookahead until
	// we made lexInsideAction not loop.
	{"long pipeline deadlock", "{{|||||}}", {
		tLeft,
		tPipe,
		tPipe,
		tPipe,
		tPipe,
		tPipe,
		tRight,
		tEOF,
	}},
	{"text with bad comment", "hello-{{/*/}}-world", {
		mk(itemText, "hello-"),
		mk(itemError, std::string(R"__(unclosed comment)__")),
	}},
	{"text with comment close separated from delim", "hello-{{/* */ }}-world", {
		mk(itemText, "hello-"),
		mk(itemError, std::string(R"__(comment ends before closing delimiter)__")),
	}},
	// This one is an error that we can't catch because it breaks templates with
	// minimized JavaScript. Should have fixed it before Go 1.1.
	{"unmatched right delimiter", "hello-{.}}-world", {
		mk(itemText, "hello-{.}}-world"),
		tEOF,
	}},
};

const item tLeftDelim = mk(itemLeftDelim, "$$");
const item tRightDelim = mk(itemRightDelim, "@@");

const std::vector<lex_test> lexDelimTests = {
	{"punctuation", "$$,@%{{}}@@", {
		tLeftDelim,
		mk(itemChar, ","),
		mk(itemChar, "@"),
		mk(itemChar, "%"),
		mk(itemChar, "{"),
		mk(itemChar, "{"),
		mk(itemChar, "}"),
		mk(itemChar, "}"),
		tRightDelim,
		tEOF,
	}},
	{"empty action", std::string(R"__($$@@)__"), {tLeftDelim, tRightDelim, tEOF}},
	{"for", std::string(R"__($$for@@)__"), {tLeftDelim, tFor, tRightDelim, tEOF}},
	{"quote", std::string(R"__($$"abc \n\t\" "@@)__"), {tLeftDelim, tQuote, tRightDelim, tEOF}},
	{"raw quote", "$$" + raw + "@@", {tLeftDelim, tRawQuote, tRightDelim, tEOF}},
};

const std::vector<lex_test> lexPosTests = {
	{"empty", "", {{itemEOF, 0, "", 1}}},
	{"punctuation", "{{,@%#}}", {
		{itemLeftDelim, 0, "{{", 1},
		{itemChar, 2, ",", 1},
		{itemChar, 3, "@", 1},
		{itemChar, 4, "%", 1},
		{itemChar, 5, "#", 1},
		{itemRightDelim, 6, "}}", 1},
		{itemEOF, 8, "", 1},
	}},
	{"sample", "0123{{hello}}xyz", {
		{itemText, 0, "0123", 1},
		{itemLeftDelim, 4, "{{", 1},
		{itemIdentifier, 6, "hello", 1},
		{itemRightDelim, 11, "}}", 1},
		{itemText, 13, "xyz", 1},
		{itemEOF, 16, "", 1},
	}},
	{"trimafter", "{{x -}}\n{{y}}", {
		{itemLeftDelim, 0, "{{", 1},
		{itemIdentifier, 2, "x", 1},
		{itemRightDelim, 5, "}}", 1},
		{itemLeftDelim, 8, "{{", 2},
		{itemIdentifier, 10, "y", 2},
		{itemRightDelim, 11, "}}", 2},
		{itemEOF, 13, "", 2},
	}},
	{"trimbefore", "{{x}}\n{{- y}}", {
		{itemLeftDelim, 0, "{{", 1},
		{itemIdentifier, 2, "x", 1},
		{itemRightDelim, 3, "}}", 1},
		{itemLeftDelim, 6, "{{", 2},
		{itemIdentifier, 10, "y", 2},
		{itemRightDelim, 11, "}}", 2},
		{itemEOF, 13, "", 2},
	}},
	{"longcomment", "{{/*\n*/}}\n{{undefinedFunction \"test\"}}", {
		{itemComment, 2, "/*\n*/", 1},
		{itemText, 9, "\n", 2},
		{itemLeftDelim, 10, "{{", 3},
		{itemIdentifier, 12, "undefinedFunction", 3},
		{itemSpace, 29, " ", 3},
		{itemString, 30, "\"test\"", 3},
		{itemRightDelim, 36, "}}", 3},
		{itemEOF, 38, "", 3},
	}},
};
}  // namespace lex_tables

const std::vector<number_test> numberTests = {
	// basics
	{"0", true, true, true, false, 0, 0, 0, 0},
	{"-0", true, true, true, false, 0, 0, 0, 0}, // check that -0 is a uint.
	{"73", true, true, true, false, 73, 73, 73, 0},
	{"7_3", true, true, true, false, 73, 73, 73, 0},
	{"0b10_010_01", true, true, true, false, 73, 73, 73, 0},
	{"0B10_010_01", true, true, true, false, 73, 73, 73, 0},
	{"073", true, true, true, false, 073, 073, 073, 0},
	{"0o73", true, true, true, false, 073, 073, 073, 0},
	{"0O73", true, true, true, false, 073, 073, 073, 0},
	{"0x73", true, true, true, false, 0x73, 0x73, 0x73, 0},
	{"0X73", true, true, true, false, 0x73, 0x73, 0x73, 0},
	{"0x7_3", true, true, true, false, 0x73, 0x73, 0x73, 0},
	{"-73", true, false, true, false, -73, 0, -73, 0},
	{"+73", true, false, true, false, 73, 0, 73, 0},
	{"100", true, true, true, false, 100, 100, 100, 0},
	{"1e9", true, true, true, false, 1000000000, 1000000000, 1000000000, 0},
	{"-1e9", true, false, true, false, -1000000000, 0, -1000000000, 0},
	{"-1.2", false, false, true, false, 0, 0, -1.2, 0},
	{"1e19", false, true, true, false, 0, 10000000000000000000ULL, 10000000000000000000ULL, 0},
	{"1e1_9", false, true, true, false, 0, 10000000000000000000ULL, 10000000000000000000ULL, 0},
	{"1E19", false, true, true, false, 0, 10000000000000000000ULL, 10000000000000000000ULL, 0},
	{"-1e19", false, false, true, false, 0, 0, -1e19, 0},
	{"0x_1p4", true, true, true, false, 16, 16, 16, 0},
	{"0X_1P4", true, true, true, false, 16, 16, 16, 0},
	{"0x_1p-4", false, false, true, false, 0, 0, 1 / 16., 0},
	{"4i", false, false, false, true, 0, 0, 0, 4i},
	{"-1.2+4.2i", false, false, false, true, 0, 0, 0, -1.2 + 4.2i},
	{"073i", false, false, false, true, 0, 0, 0, 73i}, // not octal!
	// complex with 0 imaginary are float (and maybe integer)
	{"0i", true, true, true, true, 0, 0, 0, 0},
	{"-1.2+0i", false, false, true, true, 0, 0, -1.2, -1.2},
	{"-12+0i", true, false, true, true, -12, 0, -12, -12},
	{"13+0i", true, true, true, true, 13, 13, 13, 13},
	// funny bases
	{"0123", true, true, true, false, 0123, 0123, 0123, 0},
	{"-0x0", true, true, true, false, 0, 0, 0, 0},
	{"0xdeadbeef", true, true, true, false, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0},
	// character constants
	{std::string(R"__('a')__"), true, true, true, false, 'a', 'a', 'a', 0},
	{std::string(R"__('\n')__"), true, true, true, false, '\n', '\n', '\n', 0},
	{std::string(R"__('\\')__"), true, true, true, false, '\\', '\\', '\\', 0},
	{std::string(R"__('\'')__"), true, true, true, false, '\'', '\'', '\'', 0},
	{std::string(R"__('\xFF')__"), true, true, true, false, 0xFF, 0xFF, 0xFF, 0},
	{std::string(R"__('パ')__"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, 0},
	{std::string(R"__('\u30d1')__"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, 0},
	{std::string(R"__('\U000030d1')__"), true, true, true, false, 0x30d1, 0x30d1, 0x30d1, 0},
	// some broken syntax
	{.text = "+-2"},
	{.text = "0x123."},
	{.text = "1e."},
	{.text = "0xi."},
	{.text = "1+2."},
	{.text = "'x"},
	{.text = "'xx'"},
	{.text = "'433937734937734969526500969526500'"}, // Integer too large - issue 10634.
	// Issue 8622 - 0xe parsed as floating point. Very embarrassing.
	{"0xef", true, true, true, false, 0xef, 0xef, 0xef, 0},
};

const std::vector<parse_test> parseTests = {
	{"empty", "", noError,
		std::string(R"__()__")},
	{"comment", "{{/*\n\n\n*/}}", noError,
		std::string(R"__()__")},
	{"spaces", " \t\n", noError,
		std::string(R"__(" \t\n")__")},
	{"text", "some text", noError,
		std::string(R"__("some text")__")},
	{"emptyAction", "{{}}", hasError,
		std::string(R"__({{}})__")},
	{"field", "{{.X}}", noError,
		std::string(R"__({{.X}})__")},
	{"simple command", "{{printf}}", noError,
		std::string(R"__({{printf}})__")},
	{"$ invocation", "{{$}}", noError,
		"{{$}}"},
	{"variable invocation", "{{with $x := 3}}{{$x 23}}{{end}}", noError,
		"{{with $x := 3}}{{$x 23}}{{end}}"},
	{"variable with fields", "{{$.I}}", noError,
		"{{$.I}}"},
	{"multi-word command", "{{printf `%d` 23}}", noError,
		"{{printf `%d` 23}}"},
	{"pipeline", "{{.X|.Y}}", noError,
		std::string(R"__({{.X | .Y}})__")},
	{"pipeline with decl", "{{$x := .X|.Y}}", noError,
		std::string(R"__({{$x := .X | .Y}})__")},
	{"nested pipeline", "{{.X (.Y .Z) (.A | .B .C) (.E)}}", noError,
		std::string(R"__({{.X (.Y .Z) (.A | .B .C) (.E)}})__")},
	{"field applied to parentheses", "{{(.Y .Z).Field}}", noError,
		std::string(R"__({{(.Y .Z).Field}})__")},
	{"simple if", "{{if .X}}hello{{end}}", noError,
		std::string(R"__({{if .X}}"hello"{{end}})__")},
	{"if with else", "{{if .X}}true{{else}}false{{end}}", noError,
		std::string(R"__({{if .X}}"true"{{else}}"false"{{end}})__")},
	{"if with else if", "{{if .X}}true{{else if .Y}}false{{end}}", noError,
		std::string(R"__({{if .X}}"true"{{else}}{{if .Y}}"false"{{end}}{{end}})__")},
	{"if else chain", "+{{if .X}}X{{else if .Y}}Y{{else if .Z}}Z{{end}}+", noError,
		std::string(R"__("+"{{if .X}}"X"{{else}}{{if .Y}}"Y"{{else}}{{if .Z}}"Z"{{end}}{{end}}{{end}}"+")__")},
	{"simple range", "{{range .X}}hello{{end}}", noError,
		std::string(R"__({{range .X}}"hello"{{end}})__")},
	{"chained field range", "{{range .X.Y.Z}}hello{{end}}", noError,
		std::string(R"__({{range .X.Y.Z}}"hello"{{end}})__")},
	{"nested range", "{{range .X}}hello{{range .Y}}goodbye{{end}}{{end}}", noError,
		std::string(R"__({{range .X}}"hello"{{range .Y}}"goodbye"{{end}}{{end}})__")},
	{"range with else", "{{range .X}}true{{else}}false{{end}}", noError,
		std::string(R"__({{range .X}}"true"{{else}}"false"{{end}})__")},
	{"range over pipeline", "{{range .X|.M}}true{{else}}false{{end}}", noError,
		std::string(R"__({{range .X | .M}}"true"{{else}}"false"{{end}})__")},
	{"range []int", "{{range .SI}}{{.}}{{end}}", noError,
		std::string(R"__({{range .SI}}{{.}}{{end}})__")},
	{"range 1 var", "{{range $x := .SI}}{{.}}{{end}}", noError,
		std::string(R"__({{range $x := .SI}}{{.}}{{end}})__")},
	{"range 2 vars", "{{range $x, $y := .SI}}{{.}}{{end}}", noError,
		std::string(R"__({{range $x, $y := .SI}}{{.}}{{end}})__")},
	{"range with break", "{{range .SI}}{{.}}{{break}}{{end}}", noError,
		std::string(R"__({{range .SI}}{{.}}{{break}}{{end}})__")},
	{"range with continue", "{{range .SI}}{{.}}{{continue}}{{end}}", noError,
		std::string(R"__({{range .SI}}{{.}}{{continue}}{{end}})__")},
	{"constants", "{{range .SI 1 -3.2i true false 'a' nil}}{{end}}", noError,
		std::string(R"__({{range .SI 1 -3.2i true false 'a' nil}}{{end}})__")},
	{"template", "{{template `x`}}", noError,
		std::string(R"__({{template "x"}})__")},
	{"template with arg", "{{template `x` .Y}}", noError,
		std::string(R"__({{template "x" .Y}})__")},
	{"with", "{{with .X}}hello{{end}}", noError,
		std::string(R"__({{with .X}}"hello"{{end}})__")},
	{"with with else", "{{with .X}}hello{{else}}goodbye{{end}}", noError,
		std::string(R"__({{with .X}}"hello"{{else}}"goodbye"{{end}})__")},
	{"with with else with", "{{with .X}}hello{{else with .Y}}goodbye{{end}}", noError,
		std::string(R"__({{with .X}}"hello"{{else}}{{with .Y}}"goodbye"{{end}}{{end}})__")},
	{"with else chain", "{{with .X}}X{{else with .Y}}Y{{else with .Z}}Z{{end}}", noError,
		std::string(R"__({{with .X}}"X"{{else}}{{with .Y}}"Y"{{else}}{{with .Z}}"Z"{{end}}{{end}}{{end}})__")},
	// Trimming spaces.
	{"trim left", "x \r\n\t{{- 3}}", noError, std::string(R"__("x"{{3}})__")},
	{"trim right", "{{3 -}}\n\n\ty", noError, std::string(R"__({{3}}"y")__")},
	{"trim left and right", "x \r\n\t{{- 3 -}}\n\n\ty", noError, std::string(R"__("x"{{3}}"y")__")},
	{"trim with extra spaces", "x\n{{-  3   -}}\ny", noError, std::string(R"__("x"{{3}}"y")__")},
	{"comment trim left", "x \r\n\t{{- /* hi */}}", noError, std::string(R"__("x")__")},
	{"comment trim right", "{{/* hi */ -}}\n\n\ty", noError, std::string(R"__("y")__")},
	{"comment trim left and right", "x \r\n\t{{- /* */ -}}\n\n\ty", noError, std::string(R"__("x""y")__")},
	{"block definition", std::string(R"__({{block "foo" .}}hello{{end}})__"), noError,
		std::string(R"__({{template "foo" .}})__")},

	{"newline in assignment", "{{ $x \n := \n 1 \n }}", noError, "{{$x := 1}}"},
	{"newline in empty action", "{{\n}}", hasError, "{{\n}}"},
	{"newline in pipeline", "{{\n\"x\"\n|\nprintf\n}}", noError, std::string(R"__({{"x" | printf}})__")},
	{"newline in comment", "{{/*\nhello\n*/}}", noError, ""},
	{"newline in comment", "{{-\n/*\nhello\n*/\n-}}", noError, ""},
	{"spaces around continue", "{{range .SI}}{{.}}{{ continue }}{{end}}", noError,
		std::string(R"__({{range .SI}}{{.}}{{continue}}{{end}})__")},
	{"spaces around break", "{{range .SI}}{{.}}{{ break }}{{end}}", noError,
		std::string(R"__({{range .SI}}{{.}}{{break}}{{end}})__")},

	// Errors.
	{"unclosed action", "hello{{range", hasError, ""},
	{"unmatched end", "{{end}}", hasError, ""},
	{"unmatched else", "{{else}}", hasError, ""},
	{"unmatched else after if", "{{if .X}}hello{{end}}{{else}}", hasError, ""},
	{"multiple else", "{{if .X}}1{{else}}2{{else}}3{{end}}", hasError, ""},
	{"missing end", "hello{{range .x}}", hasError, ""},
	{"missing end after else", "hello{{range .x}}{{else}}", hasError, ""},
	{"undefined function", "hello{{undefined}}", hasError, ""},
	{"undefined variable", "{{$x}}", hasError, ""},
	{"variable undefined after end", "{{with $x := 4}}{{end}}{{$x}}", hasError, ""},
	{"variable undefined in template", "{{template $v}}", hasError, ""},
	{"declare with field", "{{with $x.Y := 4}}{{end}}", hasError, ""},
	{"template with field ref", "{{template .X}}", hasError, ""},
	{"template with var", "{{template $v}}", hasError, ""},
	{"invalid punctuation", "{{printf 3, 4}}", hasError, ""},
	{"multidecl outside range", "{{with $v, $u := 3}}{{end}}", hasError, ""},
	{"too many decls in range", "{{range $u, $v, $w := 3}}{{end}}", hasError, ""},
	{"dot applied to parentheses", "{{printf (printf .).}}", hasError, ""},
	{"adjacent args", "{{printf 3`x`}}", hasError, ""},
	{"adjacent args with .", "{{printf `x`.}}", hasError, ""},
	{"extra end after if", "{{if .X}}a{{else if .Y}}b{{end}}{{end}}", hasError, ""},
	{"break outside range", "{{range .}}{{end}} {{break}}", hasError, ""},
	{"continue outside range", "{{range .}}{{end}} {{continue}}", hasError, ""},
	{"break in range else", "{{range .}}{{else}}{{break}}{{end}}", hasError, ""},
	{"continue in range else", "{{range .}}{{else}}{{continue}}{{end}}", hasError, ""},
	// Other kinds of assignments and operators aren't available yet.
	{"bug0a", "{{$x := 0}}{{$x}}", noError, "{{$x := 0}}{{$x}}"},
	{"bug0b", "{{$x += 1}}{{$x}}", hasError, ""},
	{"bug0c", "{{$x ! 2}}{{$x}}", hasError, ""},
	{"bug0d", "{{$x % 3}}{{$x}}", hasError, ""},
	// Check the parse fails for := rather than comma.
	{"bug0e", "{{range $x := $y := 3}}{{end}}", hasError, ""},
	// Another bug: variable read must ignore following punctuation.
	{"bug1a", "{{$x:=.}}{{$x!2}}", hasError, ""},                     // ! is just illegal here.
	{"bug1b", "{{$x:=.}}{{$x+2}}", hasError, ""},                     // $x+2 should not parse as ($x) (+2).
	{"bug1c", "{{$x:=.}}{{$x +2}}", noError, "{{$x := .}}{{$x +2}}"}, // It's OK with a space.
	// Check the range handles assignment vs. declaration properly.
	{"bug2a", "{{range $x := 0}}{{$x}}{{end}}", noError, "{{range $x := 0}}{{$x}}{{end}}"},
	{"bug2b", "{{range $x = 0}}{{$x}}{{end}}", noError, "{{range $x = 0}}{{$x}}{{end}}"},
	// dot following a literal value
	{"dot after integer", "{{1.E}}", hasError, ""},
	{"dot after float", "{{0.1.E}}", hasError, ""},
	{"dot after boolean", "{{true.E}}", hasError, ""},
	{"dot after char", "{{'a'.any}}", hasError, ""},
	{"dot after string", std::string(R"__({{"hello".guys}})__"), hasError, ""},
	{"dot after dot", "{{..E}}", hasError, ""},
	{"dot after nil", "{{nil.E}}", hasError, ""},
	// Wrong pipeline
	{"wrong pipeline dot", "{{12|.}}", hasError, ""},
	{"wrong pipeline number", "{{.|12|printf}}", hasError, ""},
	{"wrong pipeline string", "{{.|printf|\"error\"}}", hasError, ""},
	{"wrong pipeline char", "{{12|printf|'e'}}", hasError, ""},
	{"wrong pipeline boolean", "{{.|true}}", hasError, ""},
	{"wrong pipeline nil", "{{'c'|nil}}", hasError, ""},
	{"empty pipeline", std::string(R"__({{printf "%d" ( ) }})__"), hasError, ""},
	// Missing pipeline in block
	{"block definition", std::string(R"__({{block "foo"}}hello{{end}})__"), hasError, ""},

	// Expression nested depth tests.
	{"paren nesting normal", "{{ (( 1 )) }}", noError, "{{((1))}}"},
	{"paren nesting at limit", "{{ ((( 1 ))) }}", noError, "{{(((1)))}}"},
	{"paren nesting exceeds limit", "{{ (((( 1 )))) }}", hasError, "template: test:1: max expression depth exceeded"},
	{"paren nesting in pipeline", "{{ ((( 1 ))) | printf }}", noError, "{{(((1))) | printf}}"},
	{"paren nesting in pipeline exceeds limit", "{{ (((( 1 )))) | printf }}", hasError, "template: test:1: max expression depth exceeded"},
	{"paren nesting with other constructs", "{{ if ((( true ))) }}YES{{ end }}", noError, "{{if (((true)))}}\"YES\"{{end}}"},
	{"paren nesting with other constructs exceeds limit", "{{ if (((( true )))) }}YES{{ end }}", hasError, "template: test:1: max expression depth exceeded"},
};

const std::vector<is_empty_test> isEmptyTests = {
	{"empty", std::string(R"__()__"), true},
	{"nonempty", std::string(R"__(hello)__"), false},
	{"spaces only", " \t\n \t\n", true},
	{"comment only", "{{/* comment */}}", true},
	{"definition", std::string(R"__({{define "x"}}something{{end}})__"), true},
	{"definitions and space", "{{define `x`}}something{{end}}\n\n{{define `y`}}something{{end}}\n\n", true},
	{"definitions and text", "{{define `x`}}something{{end}}\nx\n{{define `y`}}something{{end}}\ny\n", false},
	{"definition and action", "{{define `x`}}something{{end}}{{if 3}}foo{{end}}", false},
};

const std::vector<parse_test> errorTests = {
	// Check line numbers are accurate.
	{"unclosed1",
		"line1\n{{",
		hasError, std::string(R"__(unclosed1:2: unclosed action)__")},
	{"unclosed2",
		"line1\n{{define `x`}}line2\n{{",
		hasError, std::string(R"__(unclosed2:3: unclosed action)__")},
	{"unclosed3",
		"line1\n{{\"x\"\n\"y\"\n",
		hasError, std::string(R"__(unclosed3:4: unclosed action started at unclosed3:2)__")},
	{"unclosed4",
		"{{\n\n\n\n\n",
		hasError, std::string(R"__(unclosed4:6: unclosed action started at unclosed4:1)__")},
	{"var1",
		"line1\n{{\nx\n}}",
		hasError, std::string(R"__(var1:3: function "x" not defined)__")},
	// Specific errors.
	{"function",
		"{{foo}}",
		hasError, std::string(R"__(function "foo" not defined)__")},
	{"comment1",
		"{{/*}}",
		hasError, std::string(R"__(comment1:1: unclosed comment)__")},
	{"comment2",
		"{{/*\nhello\n}}",
		hasError, std::string(R"__(comment2:1: unclosed comment)__")},
	{"lparen",
		"{{.X (1 2 3}}",
		hasError, std::string(R"__(unclosed left paren)__")},
	{"rparen",
		"{{.X 1 2 3 ) }}",
		hasError, "unexpected right paren"},
	{"rparen2",
		"{{(.X 1 2 3",
		hasError, std::string(R"__(unclosed action)__")},
	{"space",
		"{{`x`3}}",
		hasError, std::string(R"__(in operand)__")},
	{"idchar",
		"{{a#}}",
		hasError, std::string(R"__('#')__")},
	{"charconst",
		"{{'a}}",
		hasError, std::string(R"__(unterminated character constant)__")},
	{"stringconst",
		std::string(R"__({{"a}})__"),
		hasError, std::string(R"__(unterminated quoted string)__")},
	{"rawstringconst",
		"{{`a}}",
		hasError, std::string(R"__(unterminated raw quoted string)__")},
	{"number",
		"{{0xi}}",
		hasError, std::string(R"__(number syntax)__")},
	{"multidefine",
		"{{define `a`}}a{{end}}{{define `a`}}b{{end}}",
		hasError, std::string(R"__(multiple definition of template)__")},
	{"eof",
		"{{range .X}}",
		hasError, std::string(R"__(unexpected EOF)__")},
	{"variable",
		// Declare $x so it's defined, to avoid that error, and then check we don't parse a declaration.
		"{{$x := 23}}{{with $x.y := 3}}{{$x 23}}{{end}}",
		hasError, std::string(R"__(unexpected ":=")__")},
	{"multidecl",
		"{{$a,$b,$c := 23}}",
		hasError, std::string(R"__(too many declarations)__")},
	{"undefvar",
		"{{$a}}",
		hasError, std::string(R"__(undefined variable)__")},
	{"wrongdot",
		"{{true.any}}",
		hasError, std::string(R"__(unexpected . after term)__")},
	{"wrongpipeline",
		"{{12|false}}",
		hasError, std::string(R"__(non executable command in pipeline)__")},
	{"emptypipeline",
		std::string(R"__({{ ( ) }})__"),
		hasError, std::string(R"__(missing value for parenthesized pipeline)__")},
	{"multilinerawstring",
		"{{ $v := `\n` }} {{",
		hasError, std::string(R"__(multilinerawstring:2: unclosed action)__")},
	{"rangeundefvar",
		"{{range $k}}{{end}}",
		hasError, std::string(R"__(undefined variable)__")},
	{"rangeundefvars",
		"{{range $k, $v}}{{end}}",
		hasError, std::string(R"__(undefined variable)__")},
	{"rangemissingvalue1",
		"{{range $k,}}{{end}}",
		hasError, std::string(R"__(missing value for range)__")},
	{"rangemissingvalue2",
		"{{range $k, $v := }}{{end}}",
		hasError, std::string(R"__(missing value for range)__")},
	{"rangenotvariable1",
		"{{range $k, .}}{{end}}",
		hasError, std::string(R"__(range can only initialize variables)__")},
	{"rangenotvariable2",
		"{{range $k, 123 := .}}{{end}}",
		hasError, std::string(R"__(range can only initialize variables)__")},
};

std::vector<item> collect(const lex_test& t, std::string_view left, std::string_view right) {
    lexer l(string(t.name), string(t.input), left, right);
    l.options = lex_options{.emit_comment = true, .break_ok = true, .continue_ok = true};
    std::vector<item> items;
    for (;;) {
        item i = l.next_item();
        items.push_back(i);
        if (i.typ == itemEOF || i.typ == itemError) break;
    }
    return items;
}

bool equal(const std::vector<item>& i1, const std::vector<item>& i2, bool check_pos) {
    if (i1.size() != i2.size()) return false;
    for (std::size_t k = 0; k < i1.size(); k++) {
        if (i1[k].typ != i2[k].typ) return false;
        if (i1[k].val != i2[k].val) return false;
        if (check_pos && i1[k].pos != i2[k].pos) return false;
        if (check_pos && i1[k].line != i2[k].line) return false;
    }
    return true;
}

std::string show(const std::vector<item>& items) {
    std::string s = "[";
    for (auto& i : items) s += " {" + std::to_string(i.typ) + " " + std::to_string(i.pos) + " " + to_string(i) + " " + std::to_string(i.line) + "}";
    return s + " ]";
}

const FuncNames builtins = {"printf", "contains"};

// Go's tests use a small expression depth and quote text nodes.
struct test_knobs {
    int depth = max_stack_depth;
    test_knobs() {
        max_stack_depth = 3;
        text_format_q = true;
    }
    ~test_knobs() {
        max_stack_depth = depth;
        text_format_q = false;
    }
};

using tree_set = std::map<std::string, ptr<Tree>, std::less<>>;

}  // namespace

using namespace lex_tables;

TEST(TextTemplateParse, Lex) {
    for (auto& test : lexTests) {
        auto items = collect(test, "", "");
        EXPECT_TRUE(equal(items, test.items, false)) << test.name << ": got " << show(items) << " expected " << show(test.items);
    }
}

TEST(TextTemplateParse, Delims) {
    for (auto& test : lexDelimTests) {
        auto items = collect(test, "$$", "@@");
        EXPECT_TRUE(equal(items, test.items, false)) << test.name << ": got " << show(items);
    }
}

TEST(TextTemplateParse, DelimsAlphaNumeric) {
    lex_test test{"right delimiter with alphanumeric start", "{{hub .host hub}}", {
        mk(itemLeftDelim, "{{hub"), mk(itemSpace, " "), mk(itemField, ".host"), mk(itemSpace, " "),
        mk(itemRightDelim, "hub}}"), tEOF}};
    auto items = collect(test, "{{hub", "hub}}");
    EXPECT_TRUE(equal(items, test.items, false)) << show(items);
}

TEST(TextTemplateParse, DelimsAndMarkers) {
    lex_test test{"delims that look like markers", "{{- .x -}} {{- - .x - -}}", {
        mk(itemLeftDelim, "{{- "), mk(itemField, ".x"), mk(itemRightDelim, " -}}"), mk(itemLeftDelim, "{{- "),
        mk(itemField, ".x"), mk(itemRightDelim, " -}}"), tEOF}};
    auto items = collect(test, "{{- ", " -}}");
    EXPECT_TRUE(equal(items, test.items, false)) << show(items);
}

TEST(TextTemplateParse, Pos) {
    for (auto& test : lexPosTests) {
        auto items = collect(test, "", "");
        EXPECT_TRUE(equal(items, test.items, true)) << test.name << ": got " << show(items) << " expected " << show(test.items);
    }
}

TEST(TextTemplateParse, NumberParse) {
    for (auto& test : numberTests) {
        // Go asks fmt.Sscan whether the text is complex: a sign after the
        // first character and a trailing i.
        int typ = itemNumber;
        std::string_view t = test.text;
        if (t[0] == '\'') {
            typ = itemCharConstant;
        } else if (t.size() > 1 && t.back() == 'i' && t.find_first_of("+-", 1) != std::string_view::npos) {
            typ = itemComplex;
        }
        auto [n, err] = new_number_for_test(test.text, typ);
        bool ok = test.isInt || test.isUint || test.isFloat || test.isComplex;
        if (ok && err) {
            ADD_FAILURE() << "unexpected error for " << test.text << ": " << err;
            continue;
        }
        if (!ok && !err) {
            ADD_FAILURE() << "expected error for " << test.text;
            continue;
        }
        if (!ok) continue;
        EXPECT_EQ(n->IsComplex, test.isComplex) << test.text;
        if (test.isInt) {
            EXPECT_TRUE(n->IsInt) << "expected integer for " << test.text;
            EXPECT_EQ(n->Int64, test.i64) << test.text;
        } else {
            EXPECT_FALSE(n->IsInt) << "did not expect integer for " << test.text;
        }
        if (test.isUint) {
            EXPECT_TRUE(n->IsUint) << "expected unsigned integer for " << test.text;
            EXPECT_EQ(n->Uint64, test.u64) << test.text;
        } else {
            EXPECT_FALSE(n->IsUint) << "did not expect unsigned integer for " << test.text;
        }
        if (test.isFloat) {
            EXPECT_TRUE(n->IsFloat) << "expected float for " << test.text;
            EXPECT_EQ(n->Float64, test.f64) << test.text;
        } else {
            EXPECT_FALSE(n->IsFloat) << "did not expect float for " << test.text;
        }
        if (test.isComplex) {
            EXPECT_TRUE(n->IsComplex) << "expected complex for " << test.text;
            EXPECT_EQ(n->Complex128, test.c128) << test.text;
        } else {
            EXPECT_FALSE(n->IsComplex) << "did not expect complex for " << test.text;
        }
    }
}

namespace {

void test_parse(bool do_copy) {
    test_knobs knobs;
    for (auto& test : parseTests) {
        tree_set ts;
        auto [tmpl, err] = New(string(test.name))->Parse(test.input, "", "", ts, {&builtins});
        if (!err && !test.ok) {
            ADD_FAILURE() << test.name << ": expected error; got none";
            continue;
        }
        if (err && test.ok) {
            ADD_FAILURE() << test.name << ": unexpected error: " << err;
            continue;
        }
        if (err) continue;
        std::string result = do_copy ? tmpl->Root->Copy()->String().str() : tmpl->Root->String().str();
        EXPECT_EQ(result, test.result) << test.name << "=(" << test.input << ")";
    }
}

}  // namespace

TEST(TextTemplateParse, Parse) { test_parse(false); }

// Same as Parse, but we copy the node first.
TEST(TextTemplateParse, ParseCopy) { test_parse(true); }

TEST(TextTemplateParse, ParseWithComments) {
    test_knobs knobs;
    parse_test tests[] = {
        {"comment", "{{/*\n\n\n*/}}", noError, "{{/*\n\n\n*/}}"},
        {"comment trim left", "x \r\n\t{{- /* hi */}}", noError, R"("x"{{/* hi */}})"},
        {"comment trim right", "{{/* hi */ -}}\n\n\ty", noError, R"({{/* hi */}}"y")"},
        {"comment trim left and right", "x \r\n\t{{- /* */ -}}\n\n\ty", noError, R"("x"{{/* */}}"y")"},
    };
    for (auto& test : tests) {
        auto tr = New(string(test.name));
        tr->Mode = ParseComments;
        tree_set ts;
        auto [tmpl, err] = tr->Parse(test.input, "", "", ts);
        ASSERT_FALSE(err) << test.name << ": " << err;
        EXPECT_EQ(tmpl->Root->String(), test.result) << test.name;
    }
}

TEST(TextTemplateParse, DelimsStringRoundtrip) {
    // Each input is in canonical String form, so parsing it with the custom
    // delimiters and printing the tree must reproduce it.
    for (std::string_view input : {"[[.X]]", "[[/* a comment */]]", "[[if .X]]y[[else]]z[[end]]",
                                   "[[range .X]][[break]][[continue]][[end]]", "[[with .X]]y[[end]]",
                                   R"([[template "name" .]])"}) {
        auto tr = New(string::from_static("test"));
        tr->Mode = ParseComments;
        tree_set ts;
        auto [tmpl, err] = tr->Parse(input, "[[", "]]", ts);
        ASSERT_FALSE(err) << input << ": " << err;
        EXPECT_EQ(tmpl->Root->String(), input);
    }
}

TEST(TextTemplateParse, KeywordsAndFuncs) {
    // When a break function is provided, the parser treats 'break' as a
    // function, not a keyword.
    test_knobs knobs;
    std::string inp = "{{range .X}}{{break 20}}{{end}}";
    {
        FuncNames with_break = {"break"};
        tree_set ts;
        auto [tmpl, err] = New(string())->Parse(inp, "", "", ts, {&with_break});
        EXPECT_TRUE(!err && tmpl) << "with break func: unexpected error: " << err;
    }
    {
        FuncNames none;
        tree_set ts;
        auto [tmpl, err] = New(string())->Parse(inp, "", "", ts, {&none});
        EXPECT_TRUE(err && !tmpl) << "without break func: expected error; got none";
    }
}

TEST(TextTemplateParse, SkipFuncCheck) {
    test_knobs knobs;
    auto tr = New(string::from_static("skip func check"));
    tr->Mode = SkipFuncCheck;
    tree_set ts;
    auto [tmpl, err] = tr->Parse("{{fn 1 2}}", "", "", ts);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(tmpl->Root->String(), "{{fn 1 2}}");
}

TEST(TextTemplateParse, IsEmpty) {
    EXPECT_TRUE(IsEmptyTree(nullptr));
    for (auto& test : isEmptyTests) {
        tree_set ts;
        auto [tree, err] = New(string::from_static("root"))->Parse(test.input, "", "", ts);
        ASSERT_FALSE(err) << test.name << ": " << err;
        EXPECT_EQ(IsEmptyTree(tree->Root.get()), test.empty) << test.name;
    }
}

TEST(TextTemplateParse, ErrorContextWithTreeCopy) {
    tree_set ts;
    auto [tree, err] = New(string::from_static("root"))->Parse("{{if true}}{{end}}", "", "", ts);
    ASSERT_FALSE(err) << err;
    auto tree_copy = tree->Copy();
    auto [want_location, want_context] = tree->ErrorContext(*tree->Root->Nodes[0]);
    auto [got_location, got_context] = tree_copy->ErrorContext(*tree_copy->Root->Nodes[0]);
    EXPECT_EQ(got_location, want_location);
    EXPECT_EQ(got_context, want_context);
}

TEST(TextTemplateParse, Errors) {
    test_knobs knobs;
    for (auto& test : errorTests) {
        tree_set ts;
        auto [tmpl, err] = New(string(test.name))->Parse(test.input, "", "", ts);
        if (!err) {
            ADD_FAILURE() << test.name << ": expected error " << test.result << ", got nil";
            continue;
        }
        EXPECT_TRUE(strings::Contains(err.Error(), test.result)) << test.name << ": error " << err << " does not contain " << test.result;
    }
}

TEST(TextTemplateParse, Block) {
    const std::string input = R"(a{{block "inner" .}}bar{{.}}baz{{end}}b)";
    const std::string outer = R"(a{{template "inner" .}}b)";
    const std::string inner = "bar{{.}}baz";
    tree_set ts;
    auto [tmpl, err] = New(string::from_static("outer"))->Parse(input, "", "", ts);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(tmpl->Root->String(), outer);
    auto it = ts.find("inner");
    ASSERT_NE(it, ts.end()) << "block did not define template";
    EXPECT_EQ(it->second->Root->String(), inner);
}

TEST(TextTemplateParse, LineNum) {
    const int count = 3;
    std::string text;
    for (int i = 0; i < count; i++) text += "{{printf 1234}}\n";
    tree_set ts;
    auto [tree, err] = New(string::from_static("bench"))->Parse(text, "", "", ts, {&builtins});
    ASSERT_FALSE(err) << err;
    // Each line is an action followed by text: two nodes per line.
    auto& nodes = tree->Root->Nodes;
    for (std::size_t i = 0; i < nodes.size(); i += 2) {
        int line = 1 + static_cast<int>(i / 2);
        auto action = dynamic_pointer_cast<ActionNode>(nodes[i]);
        ASSERT_TRUE(action);
        EXPECT_EQ(action->Line, line);
        EXPECT_EQ(action->Pipe->Line, line);
    }
}
