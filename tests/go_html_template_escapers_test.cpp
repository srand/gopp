// Ported from Go 1.27.3's html/template/js_test.go, css_test.go,
// html_test.go, url_test.go and transition_test.go, with Go's test names:
// the escaping functions, called as the template functions the escaper
// adds, and the parsing helpers. Type names in messages are go++'s.
#include <gopp/html.hpp>
#include <gopp/html/template.hpp>
#include <gopp/unicode/utf8.hpp>

#include "html/template/internal.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <format>
#include <string>
#include <vector>

using namespace gopp;
namespace htmpl = gopp::html::tmpl;
using htmpl::Value;
using namespace htmpl::detail;

namespace {

// Calls the escaping template function named fn.
std::string call(std::string_view fn, const Value& v) {
    auto it = funcMap().find(fn);
    if (it == funcMap().end()) return "NO SUCH FUNCTION";
    return it->second.Call(std::span(&v, 1)).first.Str().str();
}

std::string call(std::string_view fn, std::string_view s) { return call(fn, Value::Of(std::string(s))); }

std::string jsStrEscaper(std::string_view s) { return call("_html_template_jsstrescaper", s); }
std::string jsRegexpEscaper(std::string_view s) { return call("_html_template_jsregexpescaper", s); }
std::string cssEscaper(std::string_view s) { return call("_html_template_cssescaper", s); }
std::string cssValueFilter(std::string_view s) { return call("_html_template_cssvaluefilter", s); }
std::string htmlNospaceEscaper(std::string_view s) { return call("_html_template_nospaceescaper", s); }
std::string urlNormalizer(std::string_view s) { return call("_html_template_urlnormalizer", s); }
std::string urlEscaper(std::string_view s) { return call("_html_template_urlescaper", s); }
std::string srcsetFilterAndEscaper(std::string_view s) { return call("_html_template_srcsetescaper", s); }

struct XY {
    int X, Y;
};

struct jsonErrType {
    std::pair<slice<byte>, gopp::error> MarshalJSON() const {
        return {slice<byte>(), errors::New("a */ b <script c </script d <!-- e <sCrIpT f </sCrIpT")};
    }
};

}  // namespace

TEST(GoHTMLTemplate, NextJsCtx) {
    struct {
        jsCtx ctx;
        std::string s;
    } tests[] = {
        // Statement terminators precede regexps.
        {jsCtxRegexp, ";"},
        // This is not airtight.
        //     ({ valueOf: function () { return 1 } } / 2)
        // is valid JavaScript but in practice, devs do not do this.
        // A block followed by a statement starting with a RegExp is
        // much more common:
        //     while (x) {...} /foo/.test(x) || panic()
        {jsCtxRegexp, "}"},
        // But member, call, grouping, and array expression terminators
        // precede div ops.
        {jsCtxDivOp, ")"},
        {jsCtxDivOp, "]"},
        // At the start of a primary expression, array, or expression
        // statement, expect a regexp.
        {jsCtxRegexp, "("},
        {jsCtxRegexp, "["},
        {jsCtxRegexp, "{"},
        // Assignment operators precede regexps as do all exclusively
        // prefix and binary operators.
        {jsCtxRegexp, "="},
        {jsCtxRegexp, "+="},
        {jsCtxRegexp, "*="},
        {jsCtxRegexp, "*"},
        {jsCtxRegexp, "!"},
        // Whether the + or - is infix or prefix, it cannot precede a
        // div op.
        {jsCtxRegexp, "+"},
        {jsCtxRegexp, "-"},
        // An incr/decr op precedes a div operator.
        // This is not airtight. In (g = ++/h/i) a regexp follows a
        // pre-increment operator, but in practice devs do not try to
        // increment or decrement regular expressions.
        // (g++/h/i) where ++ is a postfix operator on g is much more
        // common.
        {jsCtxDivOp, "--"},
        {jsCtxDivOp, "++"},
        {jsCtxDivOp, "x--"},
        // When we have many dashes or pluses, then they are grouped
        // left to right.
        {jsCtxRegexp, "x---"},
        // A postfix -- then a -.
        // return followed by a slash returns the regexp literal or the
        // slash starts a regexp literal in an expression statement that
        // is dead code.
        {jsCtxRegexp, "return"},
        {jsCtxRegexp, "return "},
        {jsCtxRegexp, "return\t"},
        {jsCtxRegexp, "return\n"},
        {jsCtxRegexp, "return "},
        {jsCtxRegexp, "yield"},
        // A keyword after property access is a property name.
        {jsCtxDivOp, "x.yield"},
        {jsCtxDivOp, R"(x?.yield)"},
        {jsCtxDivOp, "x.\nyield"},
        {jsCtxDivOp, "x.in"},
        // Identifiers can be divided and cannot validly be preceded by
        // a regular expressions. Semicolon insertion cannot happen
        // between an identifier and a regular expression on a new line
        // because the one token lookahead for semicolon insertion has
        // to conclude that it could be a div binary op and treat it as
        // such.
        {jsCtxDivOp, "x"},
        {jsCtxDivOp, "x "},
        {jsCtxDivOp, "x\t"},
        {jsCtxDivOp, "x\n"},
        {jsCtxDivOp, "x "},
        {jsCtxDivOp, "preturn"},
        // Numbers precede div ops.
        {jsCtxDivOp, "0"},
        // Dots that are part of a number are div preceders.
        {jsCtxDivOp, "0."},
        // Some JS interpreters treat NBSP as a normal space, so
        // we must too in order to properly escape things.
        {jsCtxRegexp, "= "},
    };
    for (const auto& test : tests) {
        EXPECT_EQ(nextJSCtx(test.s, jsCtxRegexp), test.ctx) << test.s;
        EXPECT_EQ(nextJSCtx(test.s, jsCtxDivOp), test.ctx) << test.s;
    }
    EXPECT_EQ(nextJSCtx("   ", jsCtxRegexp), jsCtxRegexp) << "Blank tokens";
    EXPECT_EQ(nextJSCtx("   ", jsCtxDivOp), jsCtxDivOp) << "Blank tokens";
}

TEST(GoHTMLTemplate, JSValEscaper) {
    jsonErrType jsonErr;
    struct {
        Value x;
        std::string js;
        bool skipNest;
    } tests[] = {
        {Value::Of(int(42)), " 42 ", false},
        {Value::Of(unsigned(42)), " 42 ", false},
        {Value::Of(int16(42)), " 42 ", false},
        {Value::Of(uint16(42)), " 42 ", false},
        {Value::Of(int32(-42)), " -42 ", false},
        {Value::Of(uint32(42)), " 42 ", false},
        {Value::Of(int16(-42)), " -42 ", false},
        {Value::Of(uint16(42)), " 42 ", false},
        {Value::Of(int64(-42)), " -42 ", false},
        {Value::Of(uint64(42)), " 42 ", false},
        {Value::Of(uint64(1) << 53), " 9007199254740992 ", false},
        {Value::Of((uint64(1) << 53) + 1), " 9007199254740993 ", false},
        {Value::Of(float(1.0)), " 1 ", false},
        {Value::Of(float(-1.0)), " -1 ", false},
        {Value::Of(float(0.5)), " 0.5 ", false},
        {Value::Of(float(-0.5)), " -0.5 ", false},
        {Value::Of(float(1.0) / float(256)), " 0.00390625 ", false},
        {Value::Of(float(0)), " 0 ", false},
        {Value::Of(std::copysign(0.0, -1.0)), " -0 ", false},
        {Value::Of(double(1.0)), " 1 ", false},
        {Value::Of(double(-1.0)), " -1 ", false},
        {Value::Of(double(0.5)), " 0.5 ", false},
        {Value::Of(double(-0.5)), " -0.5 ", false},
        {Value::Of(double(0)), " 0 ", false},
        {Value::Of(std::copysign(0.0, -1.0)), " -0 ", false},
        {Value::Of(std::string("")), R"("")", false},
        {Value::Of(std::string("foo")), R"("foo")", false},
        {Value::Of(std::string("\r\n  ")), R"("\r\n\u2028\u2029")", false},
        {Value::Of(std::string("\t\013")), R"("\t\u000b")", false},
        {Value::Of(XY{1, 2}), R"({"X":1,"Y":2})", false},
        {Value::Of(std::vector<Value>{}), "[]", false},
        {Value::Of(std::vector<Value>{Value::Of(42), Value::Of(std::string("foo")), Value()}), R"([42,"foo",null])", false},
        {Value::Of(std::vector<std::string>{"<!--", "</script>", "-->"}), R"(["\u003c!--","\u003c/script\u003e","--\u003e"])", false},
        {Value::Of(std::string("<!--")), R"("\u003c!--")", false},
        {Value::Of(std::string("-->")), R"("--\u003e")", false},
        {Value::Of(std::string("<![CDATA[")), R"("\u003c![CDATA[")", false},
        {Value::Of(std::string("]]>")), R"("]]\u003e")", false},
        {Value::Of(std::string("</script")), R"("\u003c/script")", false},
        {Value::Of(std::string("𝄞")), R"("𝄞")", false},
        {Value(), " null ", false},
        {Value::Of(&jsonErr), R"( /* json: error calling MarshalJSON for type jsonErrType: a * / b \x3Cscript c \x3C/script d \x3C!-- e \x3Cscript f \x3C/script */null )", true},
    };
    for (const auto& test : tests) {
        EXPECT_EQ(call("_html_template_jsvalescaper", test.x), test.js) << test.x.TypeName();
        if (test.skipNest) continue;
        Value a = Value::Of(std::vector<Value>{test.x});
        std::string trimmed = test.js;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(0, 1);
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        EXPECT_EQ(call("_html_template_jsvalescaper", a), "[" + trimmed + "]") << test.x.TypeName();
    }
}

TEST(GoHTMLTemplate, JSStrEscaper) {
    struct {
        std::string x, esc;
    } tests[] = {
        {"", ""},
        {"foo", "foo"},
        {std::string("\000", 1), R"(\u0000)"},
        {"\t", R"(\t)"},
        {"\n", R"(\n)"},
        {"\r", R"(\r)"},
        {" ", R"(\u2028)"},
        {" ", R"(\u2029)"},
        {R"(\)", R"(\\)"},
        {R"(\n)", R"(\\n)"},
        {"foo\r\nbar", R"(foo\r\nbar)"},
        // Preserve attribute boundaries.
        {R"(")", R"(\u0022)"},
        {"'", R"(\u0027)"},
        // Allow embedding in HTML without further escaping.
        {"&amp;", R"(\u0026amp;)"},
        // Prevent breaking out of text node and element boundaries.
        {"</script>", R"(\u003c\/script\u003e)"},
        {"<![CDATA[", R"(\u003c![CDATA[)"},
        {"]]>", R"(]]\u003e)"},
        // https://dev.w3.org/html5/markup/aria/syntax.html#escaping-text-span
        //   "The text in style, script, title, and textarea elements
        //   must not have an escaping text span start that is not
        //   followed by an escaping text span end."
        // Furthermore, spoofing an escaping text span end could lead
        // to different interpretation of a </script> sequence otherwise
        // masked by the escaping text span, and spoofing a start could
        // allow regular text content to be interpreted as script
        // allowing script execution via a combination of a JS string
        // injection followed by an HTML text injection.
        {"<!--", R"(\u003c!--)"},
        {"-->", R"(--\u003e)"},
        // From https://code.google.com/p/doctype/wiki/ArticleUtf7
        {
            "+ADw-script+AD4-alert(1)+ADw-/script+AD4-",
            R"(\u002bADw-script\u002bAD4-alert(1)\u002bADw-\/script\u002bAD4-)",
        },
        // Invalid UTF-8 sequence
        {"foo\240bar", "foo\240bar"},
        // Invalid unicode scalar value.
        {"foo\355\240\200bar", "foo\355\240\200bar"},
    };
    for (const auto& test : tests) EXPECT_EQ(jsStrEscaper(test.x), test.esc) << test.x;
}

TEST(GoHTMLTemplate, JSRegexpEscaper) {
    struct {
        std::string x, esc;
    } tests[] = {
        {"", R"((?:))"},
        {"foo", "foo"},
        {std::string("\000", 1), R"(\u0000)"},
        {"\t", R"(\t)"},
        {"\n", R"(\n)"},
        {"\r", R"(\r)"},
        {" ", R"(\u2028)"},
        {" ", R"(\u2029)"},
        {R"(\)", R"(\\)"},
        {R"(\n)", R"(\\n)"},
        {"foo\r\nbar", R"(foo\r\nbar)"},
        // Preserve attribute boundaries.
        {R"(")", R"(\u0022)"},
        {"'", R"(\u0027)"},
        // Allow embedding in HTML without further escaping.
        {"&amp;", R"(\u0026amp;)"},
        // Prevent breaking out of text node and element boundaries.
        {"</script>", R"(\u003c\/script\u003e)"},
        {"<![CDATA[", R"(\u003c!\[CDATA\[)"},
        {"]]>", R"(\]\]\u003e)"},
        // Escaping text spans.
        {"<!--", R"(\u003c!\-\-)"},
        {"-->", R"(\-\-\u003e)"},
        {"*", R"(\*)"},
        {"+", R"(\u002b)"},
        {R"(?)", R"(\?)"},
        {"[](){}", R"(\[\]\(\)\{\})"},
        {"$foo|x.y", R"(\$foo\|x\.y)"},
        {"x^y", R"(x\^y)"},
    };
    for (const auto& test : tests) EXPECT_EQ(jsRegexpEscaper(test.x), test.esc) << test.x;
}

TEST(GoHTMLTemplate, EscapersOnLower7AndSelectHighCodepoints) {
    std::string input = std::string("\000\001\002\003\004\005\006\007\010\t\n\013\014\r\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !\"#$%&'()*+,-./0123456789:;<=>\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\177 Ā  ﻿𝄞", 145);
    struct {
        std::string name;
        std::string (*escaper)(std::string_view);
        std::string escaped;
    } tests[] = {
        {
            "jsStrEscaper",
            jsStrEscaper,
            "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\u0008\\t\\n\\u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f !\\u0022#$%\\u0026\\u0027()*\\u002b,-.\\/0123456789:;\\u003c=\\u003e\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_\\u0060abcdefghijklmnopqrstuvwxyz{|}~\177 Ā\\u2028\\u2029﻿𝄞",
        },
        {
            "jsRegexpEscaper",
            jsRegexpEscaper,
            "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\u0008\\t\\n\\u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f !\\u0022#\\$%\\u0026\\u0027\\(\\)\\*\\u002b,\\-\\.\\/0123456789:;\\u003c=\\u003e\\\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ\\[\\\\\\]\\^_`abcdefghijklmnopqrstuvwxyz\\{\\|\\}~\177 Ā\\u2028\\u2029﻿𝄞",
        },
    };
    for (const auto& test : tests) {
        std::string s = test.escaper(input);
        if (s != test.escaped) {
            ADD_FAILURE() << test.name << " once: want\n\t" << test.escaped << "\ngot\n\t" << s;
            continue;
        }
        std::string buf;
        for (std::size_t i = 0; i < input.size();) {
            auto n = std::size_t(utf8::DecodeRuneInString(std::string_view(input).substr(i)).second);
            buf += test.escaper(std::string_view(input).substr(i, n));
            i += n;
        }
        EXPECT_EQ(buf, test.escaped) << test.name << " rune-wise";
    }
}

TEST(GoHTMLTemplate, IsJsMimeType) {
    struct {
        std::string in;
        bool out;
    } tests[] = {
        {"application/javascript;version=1.8", true},
        {"application/javascript;version=1.8;foo=bar", true},
        {"application/javascript/version=1.8", false},
        {"text/javascript", true},
        {"application/json", true},
        {"application/ld+json", true},
        {"module", true},
    };
    for (const auto& test : tests) EXPECT_EQ(isJSType(test.in), test.out) << test.in;
}

TEST(GoHTMLTemplate, EndsWithCSSKeyword) {
    struct {
        std::string css, kw;
        bool want;
    } tests[] = {
        {"", "url", false},
        {"url", "url", true},
        {"URL", "url", true},
        {"Url", "url", true},
        {"url", "important", false},
        {"important", "important", true},
        {"image-url", "url", false},
        {"imageurl", "url", false},
        {"image url", "url", true},
    };
    for (const auto& test : tests) EXPECT_EQ(endsWithCSSKeyword(test.css, test.kw), test.want) << test.css << " " << test.kw;
}

TEST(GoHTMLTemplate, IsCSSNmchar) {
    struct {
        rune r;
        bool want;
    } tests[] = {
        {0, false},
        {'0', true},
        {'9', true},
        {'A', true},
        {'Z', true},
        {'a', true},
        {'z', true},
        {'_', true},
        {'-', true},
        {':', false},
        {';', false},
        {' ', false},
        {0x7f, false},
        {0x80, true},
        {0x1234, true},
        {0xd800, false},
        {0xdc00, false},
        {0xfffe, false},
        {0x10000, true},
        {0x110000, false},
    };
    for (const auto& test : tests) EXPECT_EQ(isCSSNmchar(test.r), test.want) << test.r;
}

TEST(GoHTMLTemplate, DecodeCSS) {
    struct {
        std::string css, want;
    } tests[] = {
        {"", ""},
        {"foo", "foo"},
        {R"(foo\)", "foo"},
        {R"(foo\\)", R"(foo\)"},
        {R"(\)", ""},
        {R"(\A)", "\n"},
        {R"(\a)", "\n"},
        {R"(\0a)", "\n"},
        {R"(\00000a)", "\n"},
        {R"(\000000a)", std::string("\000a", 2)},
        {R"(\1234 5)", "ሴ5"},
        {R"(\1234\20 5)", "ሴ 5"},
        {R"(\1234\A 5)", "ሴ\n5"},
        {"\\1234\t5", "ሴ5"},
        {"\\1234\n5", "ሴ5"},
        {"\\1234\r\n5", "ሴ5"},
        {R"(\12345)", "𒍅"},
        {R"(\\)", R"(\)"},
        {R"(\\ )", R"(\ )"},
        {R"(\")", R"(")"},
        {R"(\')", "'"},
        {R"(\.)", "."},
        {R"(\. .)", ". ."},
        {
            R"(The \3c i\3equick\3c/i\3e,\d\A\3cspan style=\27 color:brown\27\3e brown\3c/span\3e  fox jumps\2028over the \3c canine class=\22lazy\22 \3e dog\3c/canine\3e)",
            "The <i>quick</i>,\r\n<span style='color:brown'>brown</span> fox jumps over the <canine class=\"lazy\">dog</canine>",
        },
    };
    for (const auto& test : tests) {
        std::string got1 = decodeCSS(test.css);
        EXPECT_EQ(got1, test.want) << test.css;
        std::string recoded = cssEscaper(got1);
        EXPECT_EQ(decodeCSS(recoded), test.want) << test.css << ": escape & decode not dual for " << recoded;
    }
}

TEST(GoHTMLTemplate, HexDecode) {
    for (int i = 0; i < 0x200000; i += 101 /* coprime with 16 */) {
        std::string s = std::format("{:x}", i);
        EXPECT_EQ(int(hexDecode(s)), i) << s;
        s = std::format("{:X}", i);
        EXPECT_EQ(int(hexDecode(s)), i) << s;
    }
}

TEST(GoHTMLTemplate, SkipCSSSpace) {
    struct {
        std::string css, want;
    } tests[] = {
        {"", ""},
        {"foo", "foo"},
        {"\n", ""},
        {"\r\n", ""},
        {"\r", ""},
        {"\t", ""},
        {" ", ""},
        {"\014", ""},
        {" foo", "foo"},
        {"  foo", " foo"},
        {R"(\20)", R"(\20)"},
    };
    for (const auto& test : tests) EXPECT_EQ(skipCSSSpace(test.css), test.want) << test.css;
}

TEST(GoHTMLTemplate, CSSEscaper) {
    std::string input = std::string("\000\001\002\003\004\005\006\007\010\t\n\013\014\r\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !\"#$%&'()*+,-./0123456789:;<=>\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\177 Ā  ﻿𝄞", 145);
    std::string want = "\\0\001\002\003\004\005\006\007\010\\9 \\a\013\\c \\d\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !\\22#$%\\26\\27\\28\\29*\\2b,-.\\2f 0123456789\\3a\\3b\\3c=\\3e\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz\\7b|\\7d~\177 Ā  ﻿𝄞";
    std::string got = cssEscaper(input);
    EXPECT_EQ(got, want) << "encode";
    EXPECT_EQ(decodeCSS(got), input) << "decode";
}

TEST(GoHTMLTemplate, CSSValueFilter) {
    struct {
        std::string css, want;
    } tests[] = {
        {"", ""},
        {"foo", "foo"},
        {"0", "0"},
        {"0px", "0px"},
        {"-5px", "-5px"},
        {"1.25in", "1.25in"},
        {"+.33em", "+.33em"},
        {"100%", "100%"},
        {"12.5%", "12.5%"},
        {".foo", ".foo"},
        {"#bar", "#bar"},
        {"corner-radius", "corner-radius"},
        {"-moz-corner-radius", "-moz-corner-radius"},
        {"#000", "#000"},
        {"#48f", "#48f"},
        {"#123456", "#123456"},
        {"U+00-FF, U+980-9FF", "U+00-FF, U+980-9FF"},
        {"color: red", "color: red"},
        {"<!--", "ZgotmplZ"},
        {"-->", "ZgotmplZ"},
        {"<![CDATA[", "ZgotmplZ"},
        {"]]>", "ZgotmplZ"},
        {"</style", "ZgotmplZ"},
        {R"(")", "ZgotmplZ"},
        {"'", "ZgotmplZ"},
        {"`", "ZgotmplZ"},
        {std::string("\000", 1), "ZgotmplZ"},
        {"/* foo */", "ZgotmplZ"},
        {"//", "ZgotmplZ"},
        {"[href=~", "ZgotmplZ"},
        {"expression(alert(1337))", "ZgotmplZ"},
        {"-expression(alert(1337))", "ZgotmplZ"},
        {"expression", "ZgotmplZ"},
        {"Expression", "ZgotmplZ"},
        {"EXPRESSION", "ZgotmplZ"},
        {"-moz-binding", "ZgotmplZ"},
        {std::string("-expr\000ession(alert(1337))", 25), "ZgotmplZ"},
        {R"(-expr\0ession(alert(1337)))", "ZgotmplZ"},
        {R"(-express\69on(alert(1337)))", "ZgotmplZ"},
        {R"(-express\69 on(alert(1337)))", "ZgotmplZ"},
        {R"(-exp\72 ession(alert(1337)))", "ZgotmplZ"},
        {R"(-exp\52 ession(alert(1337)))", "ZgotmplZ"},
        {R"(-exp\000052 ession(alert(1337)))", "ZgotmplZ"},
        {R"(-expre\0000073sion)", "-expre\0073sion"},
        {"@import url evil.css", "ZgotmplZ"},
        {"<", "ZgotmplZ"},
        {">", "ZgotmplZ"},
    };
    for (const auto& test : tests) EXPECT_EQ(cssValueFilter(test.css), test.want) << test.css;
}

TEST(GoHTMLTemplate, HTMLNospaceEscaper) {
    std::string input = std::string("\000\001\002\003\004\005\006\007\010\t\n\013\014\r\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !\"#$%&'()*+,-./0123456789:;<=>\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\177 Ā  ﻿﷬𝄞erroneous\2260", 159);
    std::string want = "&#xfffd;\001\002\003\004\005\006\007\010&#9;&#10;&#11;&#12;&#13;\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037&#32;!&#34;#$%&amp;&#39;()*&#43;,-./0123456789:;&lt;&#61;&gt;\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_&#96;abcdefghijklmnopqrstuvwxyz{|}~\177 Ā  ﻿&#xfdec;𝄞erroneous&#xfffd;0";
    std::string got = htmlNospaceEscaper(input);
    EXPECT_EQ(got, want) << "encode";

    std::string replaced;
    for (char c : input) {
        if (c == '\0' || c == '\x96') replaced += "\uFFFD";
        else replaced += c;
    }
    EXPECT_EQ(html::UnescapeString(string(got)).str(), replaced) << "decode";
}

TEST(GoHTMLTemplate, StripTags) {
    struct {
        std::string input, want;
    } tests[] = {
        {"", ""},
        {"Hello, World!", "Hello, World!"},
        {"foo&amp;bar", "foo&amp;bar"},
        {R"(Hello <a href="www.example.com/">World</a>!)", "Hello World!"},
        {"Foo <textarea>Bar</textarea> Baz", "Foo Bar Baz"},
        {"Foo <!-- Bar --> Baz", "Foo  Baz"},
        {"<", "<"},
        {"foo < bar", "foo < bar"},
        {R"(Foo<script type="text/javascript">alert(1337)</script>Bar)", "FooBar"},
        {R"(Foo<div title="1>2">Bar)", "FooBar"},
        {"I <3 Ponies!", "I <3 Ponies!"},
        {"<script>foo()</script>", ""},
    };
    for (const auto& test : tests) EXPECT_EQ(stripTags(test.input), test.want) << test.input;
}

TEST(GoHTMLTemplate, URLNormalizer) {
    struct {
        std::string url, want;
    } tests[] = {
        {"", ""},
        {
            R"(http://example.com:80/foo/bar?q=foo%20&bar=x+y#frag)",
            R"(http://example.com:80/foo/bar?q=foo%20&bar=x+y#frag)",
        },
        {" ", "%20"},
        {"%7c", "%7c"},
        {"%7C", "%7C"},
        {"%2", "%252"},
        {"%", "%25"},
        {"%z", "%25z"},
        {"/foo|bar/%5cሴ", "/foo%7cbar/%5c%e1%88%b4"},
    };
    for (const auto& test : tests) {
        EXPECT_EQ(urlNormalizer(test.url), test.want) << test.url;
        EXPECT_EQ(urlNormalizer(test.want), test.want) << "not idempotent: " << test.want;
    }
}

TEST(GoHTMLTemplate, URLFilters) {
    std::string input = std::string("\000\001\002\003\004\005\006\007\010\t\n\013\014\r\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037 !\"#$%&'()*+,-./0123456789:;<=>\077@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\177 Ā  ﻿𝄞", 145);
    struct {
        std::string name;
        std::string (*escaper)(std::string_view);
        std::string escaped;
    } tests[] = {
        {
            "urlEscaper",
            urlEscaper,
            "%00%01%02%03%04%05%06%07%08%09%0a%0b%0c%0d%0e%0f%10%11%12%13%14%15%16%17%18%19%1a%1b%1c%1d%1e%1f%20%21%22%23%24%25%26%27%28%29%2a%2b%2c-.%2f0123456789%3a%3b%3c%3d%3e%3f%40ABCDEFGHIJKLMNOPQRSTUVWXYZ%5b%5c%5d%5e_%60abcdefghijklmnopqrstuvwxyz%7b%7c%7d~%7f%c2%a0%c4%80%e2%80%a8%e2%80%a9%ef%bb%bf%f0%9d%84%9e",
        },
        {
            "urlNormalizer",
            urlNormalizer,
            R"(%00%01%02%03%04%05%06%07%08%09%0a%0b%0c%0d%0e%0f%10%11%12%13%14%15%16%17%18%19%1a%1b%1c%1d%1e%1f%20!%22#$%25&%27%28%29*+,-./0123456789:;%3c=%3e?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[%5c]%5e_%60abcdefghijklmnopqrstuvwxyz%7b%7c%7d~%7f%c2%a0%c4%80%e2%80%a8%e2%80%a9%ef%bb%bf%f0%9d%84%9e)",
        },
    };
    for (const auto& test : tests) EXPECT_EQ(test.escaper(input), test.escaped) << test.name;
}

TEST(GoHTMLTemplate, SrcsetFilter) {
    struct {
        std::string name, input, want;
    } tests[] = {
        {"one ok", "http://example.com/img.png", "http://example.com/img.png"},
        {"one ok with metadata", " /img.png 200w", " /img.png 200w"},
        {"one bad", "javascript:alert(1) 200w", "#ZgotmplZ"},
        {"two ok", "foo.png, bar.png", "foo.png, bar.png"},
        {"left bad", "javascript:alert(1), /foo.png", "#ZgotmplZ, /foo.png"},
        {"right bad", "/bogus#, javascript:alert(1)", "/bogus#,#ZgotmplZ"},
    };
    for (const auto& test : tests) EXPECT_EQ(srcsetFilterAndEscaper(test.input), test.want) << test.name;
}

TEST(GoHTMLTemplate, FindEndTag) {
    struct {
        std::string s, tag;
        int want;
    } tests[] = {
        {"", "tag", -1},
        {"hello </textarea> hello", "textarea", 6},
        {"hello </TEXTarea> hello", "textarea", 6},
        {"hello </textAREA>", "textarea", 6},
        {"hello </textarea", "textareax", -1},
        {"hello </textarea>", "tag", -1},
        {"hello tag </textarea", "tag", -1},
        {"hello </tag> </other> </textarea> <other>", "textarea", 22},
        {"</textarea> <other>", "textarea", 0},
        {"<div> </div> </TEXTAREA>", "textarea", 13},
        {"<div> </div> </TEXTAREA\t>", "textarea", 13},
        {"<div> </div> </TEXTAREA >", "textarea", 13},
        {"<div> </div> </TEXTAREAfoo", "textarea", -1},
        {"</TEXTAREAfoo </textarea>", "textarea", 14},
        {"<</script >", "script", 1},
        {"</script>", "textarea", -1},
    };
    for (const auto& test : tests) {
        std::size_t got = indexTagEnd(test.s, test.tag);
        EXPECT_EQ(got == std::string_view::npos ? -1 : int(got), test.want) << test.s << "/" << test.tag;
    }
}
