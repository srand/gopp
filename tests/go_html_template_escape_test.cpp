// Ported from Go 1.27.3's html/template/escape_test.go, with Go's test
// names. Differences: type names in messages are go++'s (badMarshaler, not
// *template.badMarshaler); Go's GODEBUG htmlmetacontenturlescape doesn't
// exist, so TestMetaContentEscapeGODEBUG is left out; TestRedundantFuncs
// calls the escapers through the template functions; TestEscapeText and
// TestEnsurePipelineContains reach the escaper's internals through test
// hooks; and Go's recovered panics are death tests.
#include <gopp/bytes.hpp>
#include <gopp/html/template.hpp>
#include <gopp/strings.hpp>

#include "html/template/internal.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace gopp;
namespace htmpl = gopp::html::tmpl;
namespace ttmpl = gopp::text::tmpl;
using htmpl::Value;
using namespace htmpl::detail;
namespace parse = gopp::text::tmpl::parse;

namespace {

struct badMarshaler {
    std::pair<slice<byte>, gopp::error> MarshalJSON() const {
        // Keys in valid JSON must be double quoted as must all strings.
        return {toBytes("{ foo: 'not quite valid JSON' }"), nullptr};
    }
};

struct goodMarshaler {
    std::pair<slice<byte>, gopp::error> MarshalJSON() const { return {toBytes(R"({ "<foo>": "O'Reilly" })"), nullptr}; }
};

struct escapeData {
    bool F = false, T = true;
    string C = "<Cincinnati>", G = "<Goodbye>", H = "<Hello>", I = "${ asd `` }";
    std::vector<string> A = {"<a>", "<b>"}, E = {};
    const badMarshaler* B = nullptr;
    const goodMarshaler* M = nullptr;
    int N = 42;
    Value U;                 // untyped nil
    const int* Z = nullptr;  // typed nil
    htmpl::HTML W{R"(&iexcl;<b class="foo">Hello</b>, <textarea>O'World</textarea>!)"};
};

struct escapeTest {
    const char* name;
    std::string input;
    std::string output;
};

// clang-format off
const escapeTest escapeTests[] = {
    {"if", "{{if .T}}Hello{{end}}, {{.C}}!", "Hello, &lt;Cincinnati&gt;!"},
    {"else", "{{if .F}}{{.H}}{{else}}{{.G}}{{end}}!", "&lt;Goodbye&gt;!"},
    {"overescaping1", "Hello, {{.C | html}}!", "Hello, &lt;Cincinnati&gt;!"},
    {"overescaping2", "Hello, {{html .C}}!", "Hello, &lt;Cincinnati&gt;!"},
    {
        "overescaping3",
        "{{with .C}}{{$msg := .}}Hello, {{$msg}}!{{end}}",
        "Hello, &lt;Cincinnati&gt;!",
    },
    {"assignment", "{{if $x := .H}}{{$x}}{{end}}", "&lt;Hello&gt;"},
    {"withBody", "{{with .H}}{{.}}{{end}}", "&lt;Hello&gt;"},
    {"withElse", "{{with .E}}{{.}}{{else}}{{.H}}{{end}}", "&lt;Hello&gt;"},
    {"rangeBody", "{{range .A}}{{.}}{{end}}", "&lt;a&gt;&lt;b&gt;"},
    {"rangeElse", "{{range .E}}{{.}}{{else}}{{.H}}{{end}}", "&lt;Hello&gt;"},
    {"nonStringValue", "{{.T}}", "true"},
    {"untypedNilValue", "{{.U}}", ""},
    {"typedNilValue", "{{.Z}}", "&lt;nil&gt;"},
    {"constant", R"(<a href="/search?q={{"'a<b'"}}">)", R"(<a href="/search?q=%27a%3cb%27">)"},
    {"multipleAttrs", "<a b=1 c={{.H}}>", "<a b=1 c=&lt;Hello&gt;>"},
    {"urlStartRel", R"(<a href='{{"/foo/bar?a=b&c=d"}}'>)", R"(<a href='/foo/bar?a=b&amp;c=d'>)"},
    {
        "urlStartAbsOk",
        R"(<a href='{{"http://example.com/foo/bar?a=b&c=d"}}'>)",
        R"(<a href='http://example.com/foo/bar?a=b&amp;c=d'>)",
    },
    {
        "protocolRelativeURLStart",
        R"(<a href='{{"//example.com:8000/foo/bar?a=b&c=d"}}'>)",
        R"(<a href='//example.com:8000/foo/bar?a=b&amp;c=d'>)",
    },
    {
        "pathRelativeURLStart",
        R"(<a href="{{"/javascript:80/foo/bar"}}">)",
        R"(<a href="/javascript:80/foo/bar">)",
    },
    {
        "dangerousURLStart",
        "<a href='{{\"javascript:alert(%22pwned%22)\"}}'>",
        "<a href='#ZgotmplZ'>",
    },
    {
        "dangerousURLStart2",
        "<a href='  {{\"javascript:alert(%22pwned%22)\"}}'>",
        "<a href='  #ZgotmplZ'>",
    },
    {
        "nonHierURL",
        R"(<a href={{"mailto:Muhammed \"The Greatest\" Ali <m.ali@example.com>"}}>)",
        "<a href=mailto:Muhammed%20%22The%20Greatest%22%20Ali%20%3cm.ali@example.com%3e>",
    },
    {
        "urlPath",
        R"(<a href='http://{{"javascript:80"}}/foo'>)",
        "<a href='http://javascript:80/foo'>",
    },
    {"urlQuery", R"(<a href='/search?q={{.H}}'>)", R"(<a href='/search?q=%3cHello%3e'>)"},
    {"urlFragment", "<a href='/faq#{{.H}}'>", "<a href='/faq#%3cHello%3e'>"},
    {"urlBranch", R"(<a href="{{if .F}}/foo?a=b{{else}}/bar{{end}}">)", R"(<a href="/bar">)"},
    {
        "urlBranchConflictMoot",
        R"(<a href="{{if .T}}/foo?a={{else}}/bar#{{end}}{{.C}}">)",
        R"(<a href="/foo?a=%3cCincinnati%3e">)",
    },
    {
        "jsStrValue",
        "<button onclick='alert({{.H}})'>",
        R"(<button onclick='alert(&#34;\u003cHello\u003e&#34;)'>)",
    },
    {"jsNumericValue", "<button onclick='alert({{.N}})'>", "<button onclick='alert( 42 )'>"},
    {"jsBoolValue", "<button onclick='alert({{.T}})'>", "<button onclick='alert( true )'>"},
    {
        "jsNilValueTyped",
        "<button onclick='alert(typeof{{.Z}})'>",
        "<button onclick='alert(typeof null )'>",
    },
    {
        "jsNilValueUntyped",
        "<button onclick='alert(typeof{{.U}})'>",
        "<button onclick='alert(typeof null )'>",
    },
    {
        "jsObjValue",
        "<button onclick='alert({{.A}})'>",
        R"(<button onclick='alert([&#34;\u003ca\u003e&#34;,&#34;\u003cb\u003e&#34;])'>)",
    },
    {
        "jsObjValueScript",
        "<script>alert({{.A}})</script>",
        R"(<script>alert(["\u003ca\u003e","\u003cb\u003e"])</script>)",
    },
    {
        "scriptTypeSpace",
        R"(<script type=" ">{{.H}}</script>)",
        R"(<script type=" ">"\u003cHello\u003e"</script>)",
    },
    {
        "scriptTypeTab",
        "<script type=\"\t\">{{.H}}</script>",
        "<script type=\"\t\">\"\\u003cHello\\u003e\"</script>",
    },
    {
        "scriptTypeEmpty",
        R"(<script type="">{{.H}}</script>)",
        R"(<script type="">"\u003cHello\u003e"</script>)",
    },
    {
        "jsObjValueNotOverEscaped",
        "<button onclick='alert({{.A | html}})'>",
        R"(<button onclick='alert([&#34;\u003ca\u003e&#34;,&#34;\u003cb\u003e&#34;])'>)",
    },
    {
        "jsStr",
        "<button onclick='alert(&quot;{{.H}}&quot;)'>",
        R"(<button onclick='alert(&quot;\u003cHello\u003e&quot;)'>)",
    },
    {
        "badMarshaler",
        "<button onclick='alert(1/{{.B}}in numbers)'>",
        "<button onclick='alert(1/ /* json: error calling MarshalJSON for type badMarshaler: invalid character &#39;f&#39; looking for beginning of object key string */null in numbers)'>",
    },
    {
        "jsMarshaler",
        "<button onclick='alert({{.M}})'>",
        R"(<button onclick='alert({&#34;\u003cfoo\u003e&#34;:&#34;O&#39;Reilly&#34;})'>)",
    },
    {
        "jsStrNotUnderEscaped",
        "<button onclick='alert({{.C | urlquery}})'>",
        // URL escaped, then quoted for JS.
        "<button onclick='alert(&#34;%3CCincinnati%3E&#34;)'>",
    },
    {
        "jsRe",
        R"(<button onclick='alert(/{{"foo+bar"}}/.test(""))'>)",
        R"(<button onclick='alert(/foo\u002bbar/.test(""))'>)",
    },
    {
        "jsReBlank",
        R"(<script>alert(/{{""}}/.test(""));</script>)",
        R"(<script>alert(/(?:)/.test(""));</script>)",
    },
    {
        "jsReAmbigOk",
        "<script>{{if true}}var x = 1{{end}}</script>",
        // The {if} ends in an ambiguous jsCtx but there is
        // no slash following so we shouldn't care.
        "<script>var x = 1</script>",
    },
    {"styleBidiKeywordPassed", R"(<p style="dir: {{"ltr"}}">)", R"(<p style="dir: ltr">)"},
    {
        "styleBidiPropNamePassed",
        R"(<p style="border-{{"left"}}: 0; border-{{"right"}}: 1in">)",
        R"(<p style="border-left: 0; border-right: 1in">)",
    },
    {
        "styleExpressionBlocked",
        "<p style=\"width: {{\"expression(alert(1337))\"}}\">",
        R"(<p style="width: ZgotmplZ">)",
    },
    {
        "styleTagSelectorPassed",
        R"(<style>{{"p"}} { color: pink }</style>)",
        "<style>p { color: pink }</style>",
    },
    {
        "styleIDPassed",
        R"(<style>p{{"#my-ID"}} { font: Arial }</style>)",
        "<style>p#my-ID { font: Arial }</style>",
    },
    {
        "styleClassPassed",
        R"(<style>p{{".my_class"}} { font: Arial }</style>)",
        "<style>p.my_class { font: Arial }</style>",
    },
    {
        "styleQuantityPassed",
        R"(<a style="left: {{"2em"}}; top: {{0}}">)",
        R"(<a style="left: 2em; top: 0">)",
    },
    {"stylePctPassed", R"(<table style=width:{{"100%"}}>)", "<table style=width:100%>"},
    {
        "styleColorPassed",
        R"(<p style="color: {{"#8ff"}}; background: {{"#000"}}">)",
        R"(<p style="color: #8ff; background: #000">)",
    },
    {
        "styleObfuscatedExpressionBlocked",
        "<p style=\"width: {{\"  e\\\\78preS\\x00Sio/**/n(alert(1337))\"}}\">",
        R"(<p style="width: ZgotmplZ">)",
    },
    {
        "styleMozBindingBlocked",
        "<p style=\"{{\"-moz-binding(alert(1337))\"}}: ...\">",
        R"(<p style="ZgotmplZ: ...">)",
    },
    {
        "styleObfuscatedMozBindingBlocked",
        "<p style=\"{{\"  -mo\\\\7a-B\\x00I/**/nding(alert(1337))\"}}: ...\">",
        R"(<p style="ZgotmplZ: ...">)",
    },
    {
        "styleFontNameString",
        R"(<p style='font-family: "{{"Times New Roman"}}"'>)",
        R"(<p style='font-family: "Times New Roman"'>)",
    },
    {
        "styleFontNameString",
        R"(<p style='font-family: "{{"Times New Roman"}}", "{{"sans-serif"}}"'>)",
        R"(<p style='font-family: "Times New Roman", "sans-serif"'>)",
    },
    {
        "styleFontNameUnquoted",
        R"(<p style='font-family: {{"Times New Roman"}}'>)",
        "<p style='font-family: Times New Roman'>",
    },
    {
        "styleURLQueryEncoded",
        "<p style=\"background: url(/img\077name={{\"O'Reilly Animal(1)<2>.png\"}})\">",
        "<p style=\"background: url(/img\077name=O%27Reilly%20Animal%281%29%3c2%3e.png)\">",
    },
    {
        "styleQuotedURLQueryEncoded",
        "<p style=\"background: url('/img\077name={{\"O'Reilly Animal(1)<2>.png\"}}')\">",
        "<p style=\"background: url('/img\077name=O%27Reilly%20Animal%281%29%3c2%3e.png')\">",
    },
    {
        "styleStrQueryEncoded",
        R"(<p style="background: '/img?name={{"O'Reilly Animal(1)<2>.png"}}'">)",
        R"(<p style="background: '/img?name=O%27Reilly%20Animal%281%29%3c2%3e.png'">)",
    },
    {
        "styleURLBadProtocolBlocked",
        "<a style=\"background: url('{{\"javascript:alert(1337)\"}}')\">",
        "<a style=\"background: url('#ZgotmplZ')\">",
    },
    {
        "styleStrBadProtocolBlocked",
        "<a style=\"background: '{{\"vbscript:alert(1337)\"}}'\">",
        R"(<a style="background: '#ZgotmplZ'">)",
    },
    {
        "styleStrEncodedProtocolEncoded",
        "<a style=\"background: '{{\"javascript\\\\3a alert(1337)\"}}'\">",
        // The CSS string 'javascript\\3a alert(1337)' does not contain a colon.
        R"(<a style="background: 'javascript\\3a alert\28 1337\29 '">)",
    },
    {
        "styleURLGoodProtocolPassed",
        "<a style=\"background: url('{{\"http://oreilly.com/O'Reilly Animals(1)<2>;{}.html\"}}')\">",
        "<a style=\"background: url('http://oreilly.com/O%27Reilly%20Animals%281%29%3c2%3e;%7b%7d.html')\">",
    },
    {
        "styleStrGoodProtocolPassed",
        R"(<a style="background: '{{"http://oreilly.com/O'Reilly Animals(1)<2>;{}.html"}}'">)",
        R"(<a style="background: 'http\3a\2f\2foreilly.com\2fO\27Reilly Animals\28 1\29\3c 2\3e\3b\7b\7d.html'">)",
    },
    {
        "styleURLEncodedForHTMLInAttr",
        "<a style=\"background: url('{{\"/search\077img=foo&size=icon\"}}')\">",
        "<a style=\"background: url('/search\077img=foo&amp;size=icon')\">",
    },
    {
        "styleURLNotEncodedForHTMLInCdata",
        R"(<style>body { background: url('{{"/search?img=foo&size=icon"}}') }</style>)",
        R"(<style>body { background: url('/search?img=foo&size=icon') }</style>)",
    },
    {
        "styleURLMixedCase",
        "<p style=\"background: URL(#{{.H}})\">",
        "<p style=\"background: URL(#%3cHello%3e)\">",
    },
    {"stylePropertyPairPassed", R"(<a style='{{"color: red"}}'>)", "<a style='color: red'>"},
    {
        "styleStrSpecialsEncoded",
        R"(<a style="font-family: '{{"/**/'\";:// \\"}}', &quot;{{"/**/'\";:// \\"}}&quot;">)",
        R"(<a style="font-family: '\2f**\2f\27\22\3b\3a\2f\2f  \\', &quot;\2f**\2f\27\22\3b\3a\2f\2f  \\&quot;">)",
    },
    {
        "styleURLSpecialsEncoded",
        R"(<a style="border-image: url({{"/**/'\";:// \\"}}), url(&quot;{{"/**/'\";:// \\"}}&quot;), url('{{"/**/'\";:// \\"}}'), 'http://www.example.com/?q={{"/**/'\";:// \\"}}''">)",
        R"(<a style="border-image: url(/**/%27%22;://%20%5c), url(&quot;/**/%27%22;://%20%5c&quot;), url('/**/%27%22;://%20%5c'), 'http://www.example.com/?q=%2f%2a%2a%2f%27%22%3b%3a%2f%2f%20%5c''">)",
    },
    {
        "HTML comment",
        "<b>Hello, <!-- name of world -->{{.C}}</b>",
        "<b>Hello, &lt;Cincinnati&gt;</b>",
    },
    {"HTML comment not first < in text node.", "<<!-- -->!--", "&lt;!--"},
    {"HTML normalization 1", "a < b", "a &lt; b"},
    {"HTML normalization 2", "a << b", "a &lt;&lt; b"},
    {"HTML normalization 3", "a<<!-- --><!-- -->b", "a&lt;b"},
    {"HTML doctype not normalized", "<!DOCTYPE html>Hello, World!", "<!DOCTYPE html>Hello, World!"},
    {
        "HTML doctype not case-insensitive",
        "<!doCtYPE htMl>Hello, World!",
        "<!doCtYPE htMl>Hello, World!",
    },
    {"No doctype injection", R"(<!{{"DOCTYPE"}})", "&lt;!DOCTYPE"},
    {
        "Split HTML comment",
        "<b>Hello, <!-- name of {{if .T}}city -->{{.C}}{{else}}world -->{{.W}}{{end}}</b>",
        "<b>Hello, &lt;Cincinnati&gt;</b>",
    },
    {
        "JS line comment",
        "<script>for (;;) { if (c()) break// foo not a label\nfoo({{.T}});}</script>",
        "<script>for (;;) { if (c()) break\nfoo( true );}</script>",
    },
    {
        "JS multiline block comment",
        "<script>for (;;) { if (c()) break/* foo not a label\n */foo({{.T}});}</script>",
        // Newline separates break from call. If newline
        // removed, then break will consume label leaving
        // code invalid.
        "<script>for (;;) { if (c()) break\nfoo( true );}</script>",
    },
    {
        "JS single-line block comment",
        "<script>for (;;) {\nif (c()) break/* foo a label */foo;x({{.T}});}</script>",
        // Newline separates break from call. If newline
        // removed, then break will consume label leaving
        // code invalid.
        "<script>for (;;) {\nif (c()) break foo;x( true );}</script>",
    },
    {
        "JS block comment flush with mathematical division",
        "<script>var a/*b*//c\nd</script>",
        "<script>var a /c\nd</script>",
    },
    {"JS mixed comments", "<script>var a/*b*///c\nd</script>", "<script>var a \nd</script>"},
    {
        "JS HTML-like comments",
        "<script>before <!-- beep\nbetween\nbefore-->boop\n</script>",
        "<script>before \nbetween\nbefore\n</script>",
    },
    {"JS hashbang comment", "<script>#! beep\n</script>", "<script>\n</script>"},
    {
        "Special tags in <script> string literals",
        R"(<script>var a = "asd < 123 <!-- 456 < fgh <script jkl < 789 </script"</script>)",
        R"(<script>var a = "asd < 123 \x3C!-- 456 < fgh \x3Cscript jkl < 789 \x3C/script"</script>)",
    },
    {
        "Special tags in <script> string literals (mixed case)",
        R"(<script>var a = "<!-- <ScripT </ScripT"</script>)",
        R"(<script>var a = "\x3C!-- \x3CScripT \x3C/ScripT"</script>)",
    },
    {
        "Special tags in <script> regex literals (mixed case)",
        "<script>var a = /<!-- <ScripT </ScripT/</script>",
        R"(<script>var a = /\x3C!-- \x3CScripT \x3C/ScripT/</script>)",
    },
    {
        "CSS comments",
        "<style>p// paragraph\n{border: 1px/* color */{{\"#00f\"}}}</style>",
        "<style>p\n{border: 1px #00f}</style>",
    },
    {
        "JS attr block comment",
        R"(<a onclick="f(&quot;&quot;); /* alert({{.H}}) */">)",
        // Attribute comment tests should pass if the comments
        // are successfully elided.
        R"(<a onclick="f(&quot;&quot;); /* alert() */">)",
    },
    {"JS attr line comment", "<a onclick=\"// alert({{.G}})\">", "<a onclick=\"// alert()\">"},
    {
        "CSS attr block comment",
        R"(<a style="/* color: {{.H}} */">)",
        R"(<a style="/* color:  */">)",
    },
    {"CSS attr line comment", R"(<a style="// color: {{.G}}">)", R"(<a style="// color: ">)"},
    {"HTML substitution commented out", "<p><!-- {{.H}} --></p>", "<p></p>"},
    {
        "Comment ends flush with start",
        "<!--{{.}}--><script>/*{{.}}*///{{.}}\n</script><style>/*{{.}}*///{{.}}\n</style><a onclick='/*{{.}}*///{{.}}' style='/*{{.}}*///{{.}}'>",
        "<script> \n</script><style> \n</style><a onclick='/**///' style='/**///'>",
    },
    {
        "typed HTML in text",
        "{{.W}}",
        R"(&iexcl;<b class="foo">Hello</b>, <textarea>O'World</textarea>!)",
    },
    {
        "typed HTML in attribute",
        R"(<div title="{{.W}}">)",
        R"(<div title="&iexcl;Hello, O&#39;World!">)",
    },
    {
        "typed HTML in script",
        "<button onclick=\"alert({{.W}})\">",
        "<button onclick=\"alert(&#34;\\u0026iexcl;\\u003cb class=\\&#34;foo\\&#34;\\u003eHello\\u003c/b\\u003e, \\u003ctextarea\\u003eO&#39;World\\u003c/textarea\\u003e!&#34;)\">",
    },
    {
        "typed HTML in RCDATA",
        "<textarea>{{.W}}</textarea>",
        "<textarea>&iexcl;&lt;b class=&#34;foo&#34;&gt;Hello&lt;/b&gt;, &lt;textarea&gt;O&#39;World&lt;/textarea&gt;!</textarea>",
    },
    {
        "range in textarea",
        "<textarea>{{range .A}}{{.}}{{end}}</textarea>",
        "<textarea>&lt;a&gt;&lt;b&gt;</textarea>",
    },
    {
        "No tag injection",
        R"({{"10$"}}<{{"script src,evil.org/pwnd.js"}}...)",
        "10$&lt;script src,evil.org/pwnd.js...",
    },
    {"No comment injection", R"(<{{"!--"}})", "&lt;!--"},
    {
        "No RCDATA end tag injection",
        R"(<textarea><{{"/textarea "}}...</textarea>)",
        "<textarea>&lt;/textarea ...</textarea>",
    },
    {
        "optional attrs",
        R"(<img class="{{"iconClass"}}"{{if .T}} id="{{"<iconId>"}}"{{end}} src={{if .T}}"?{{"<iconPath>"}}"{{else}}"images/cleardot.gif"{{end}}{{if .T}}title="{{"<title>"}}"{{end}} alt="{{if .T}}{{"<alt>"}}{{else}}{{if .F}}{{"<title>"}}{{end}}{{end}}">)",
        R"(<img class="iconClass" id="&lt;iconId&gt;" src="?%3ciconPath%3e"title="&lt;title&gt;" alt="&lt;alt&gt;">)",
    },
    {
        "conditional valueless attr name",
        "<input{{if .T}} checked{{end}} name=n>",
        "<input checked name=n>",
    },
    {
        "conditional dynamic valueless attr name 1",
        R"(<input{{if .T}} {{"checked"}}{{end}} name=n>)",
        "<input checked name=n>",
    },
    {
        "conditional dynamic valueless attr name 2",
        R"(<input {{if .T}}{{"checked"}} {{end}}name=n>)",
        "<input checked name=n>",
    },
    {
        "dynamic attribute name",
        "<img on{{\"load\"}}=\"alert({{\"loaded\"}})\">",
        // Treated as JS since quotes are inserted.
        "<img onload=\"alert(&#34;loaded&#34;)\">",
    },
    {
        "bad dynamic attribute name 1",
        // Allow checked, selected, disabled, but not JS or
        // CSS attributes.
        "<input {{\"onchange\"}}=\"{{\"doEvil()\"}}\">",
        "<input ZgotmplZ=\"doEvil()\">",
    },
    {
        "bad dynamic attribute name 2",
        "<div {{\"sTyle\"}}=\"{{\"color: expression(alert(1337))\"}}\">",
        "<div ZgotmplZ=\"color: expression(alert(1337))\">",
    },
    {
        "bad dynamic attribute name 3",
        // Allow title or alt, but not a URL.
        "<img {{\"src\"}}=\"{{\"javascript:doEvil()\"}}\">",
        "<img ZgotmplZ=\"javascript:doEvil()\">",
    },
    {
        "bad dynamic attribute name 4",
        // Structure preservation requires values to associate
        // with a consistent attribute.
        R"(<input checked {{""}}="Whose value am I?">)",
        R"(<input checked ZgotmplZ="Whose value am I?">)",
    },
    {
        "dynamic element name",
        R"(<h{{3}}><table><t{{"head"}}>...</h{{3}}>)",
        "<h3><table><thead>...</h3>",
    },
    {
        "bad dynamic element name",
        // Dynamic element names are typically used to switch
        // between (thead, tfoot, tbody), (ul, ol), (th, td),
        // and other replaceable sets.
        // We do not currently easily support (ul, ol).
        // If we do change to support that, this test should
        // catch failures to filter out special tag names which
        // would violate the structure preservation property --
        // if any special tag name could be substituted, then
        // the content could be raw text/RCDATA for some inputs
        // and regular HTML content for others.
        "<{{\"script\"}}>{{\"doEvil()\"}}</{{\"script\"}}>",
        "&lt;script>doEvil()&lt;/script>",
    },
    {
        "srcset bad URL in second position",
        "<img srcset=\"{{\"/not-an-image#,javascript:alert(1)\"}}\">",
        // The second URL is also filtered.
        R"(<img srcset="/not-an-image#,#ZgotmplZ">)",
    },
    {
        "srcset buffer growth",
        R"(<img srcset={{",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"}}>)",
        "<img srcset=,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,>",
    },
    {"unquoted empty attribute value (plaintext)", "<p name={{.U}}>", "<p name=ZgotmplZ>"},
    {"unquoted empty attribute value (url)", "<p href={{.U}}>", "<p href=ZgotmplZ>"},
    {"quoted empty attribute value", R"(<p name="{{.U}}">)", R"(<p name="">)"},
    {
        "JS template lit special characters",
        "<script>var a = `{{.I}}`</script>",
        R"(<script>var a = `\u0024\u007b asd \u0060\u0060 \u007d`</script>)",
    },
    {
        "JS template lit special characters, nested lit",
        "<script>var a = `${ `{{.I}}` }`</script>",
        R"(<script>var a = `${ `\u0024\u007b asd \u0060\u0060 \u007d` }`</script>)",
    },
    {
        "JS template lit, nested JS",
        R"(<script>var a = `${ var a = "{{"a \" d"}}" }`</script>)",
        R"(<script>var a = `${ var a = "a \u0022 d" }`</script>)",
    },
    {
        "meta content attribute url",
        "<meta http-equiv=\"refresh\" content=\"asd; url={{\"javascript:alert(1)\"}}; asd; url={{\"vbscript:alert(1)\"}}; asd\">",
        R"(<meta http-equiv="refresh" content="asd; url=#ZgotmplZ; asd; url=#ZgotmplZ; asd">)",
    },
    {
        "meta content string",
        R"(<meta http-equiv="refresh" content="{{"asd: 123"}}">)",
        R"(<meta http-equiv="refresh" content="asd: 123">)",
    },
    {
        "meta content url with whitespace before equals",
        "<meta http-equiv=\"refresh\" content=\"0;url ={{\"javascript:alert(1)\"}}\">",
        R"(<meta http-equiv="refresh" content="0;url =#ZgotmplZ">)",
    },
    {
        "meta content url with tab before equals",
        "<meta http-equiv=\"refresh\" content=\"0;url\t={{\"javascript:alert(1)\"}}\">",
        "<meta http-equiv=\"refresh\" content=\"0;url\t=#ZgotmplZ\">",
    },
    {
        "meta content url with space after equals",
        "<meta http-equiv=\"refresh\" content=\"0;url= {{\"javascript:alert(1)\"}}\">",
        R"(<meta http-equiv="refresh" content="0;url= #ZgotmplZ">)",
    },
    {
        "meta content url with whitespace both sides of equals",
        "<meta http-equiv=\"refresh\" content=\"0;url \t= {{\"javascript:alert(1)\"}}\">",
        "<meta http-equiv=\"refresh\" content=\"0;url \t= #ZgotmplZ\">",
    },
};
// clang-format on

std::string str(const gopp::error& err) { return err ? err.Error().str() : ""; }

}  // namespace

TEST(GoHTMLTemplate, Escape) {
    badMarshaler bad;
    goodMarshaler good;
    escapeData data;
    data.B = &bad;
    data.M = &good;
    const escapeData* pdata = &data;

    for (const auto& test : escapeTests) {
        SCOPED_TRACE(test.name);
        auto tmpl = htmpl::New(test.name);
        tmpl = htmpl::Must(tmpl->Parse(test.input));
        // Check for bug 6459: Tree field was not set in Parse.
        ASSERT_EQ(tmpl->Tree, tmpl->text_->Tree) << "tree not set properly";
        strings::Builder b;
        auto err = tmpl->Execute(b, data);
        ASSERT_EQ(err, nullptr) << "template execution failed: " << str(err);
        EXPECT_EQ(b.str().str(), test.output) << "escaped output";
        strings::Builder bp;
        err = tmpl->Execute(bp, pdata);
        ASSERT_EQ(err, nullptr) << "template execution failed for pointer: " << str(err);
        EXPECT_EQ(bp.str().str(), test.output) << "escaped output for pointer";
        ASSERT_EQ(tmpl->Tree, tmpl->text_->Tree) << "tree mismatch";
    }
}

TEST(GoHTMLTemplate, EscapeMap) {
    std::map<std::string, std::string> data = {
        {"html", "<h1>Hi!</h1>"},
        {"urlquery", "http://www.foo.com/index.html?title=main"},
    };
    struct {
        const char *desc, *input, *output;
    } tests[] = {
        // covering issue 20323
        {"field with predefined escaper name 1", "{{.html | print}}", "&lt;h1&gt;Hi!&lt;/h1&gt;"},
        // covering issue 20323
        {"field with predefined escaper name 2", "{{.urlquery | print}}", "http://www.foo.com/index.html?title=main"},
    };
    for (const auto& test : tests) {
        auto tmpl = htmpl::Must(htmpl::New("")->Parse(test.input));
        strings::Builder b;
        auto err = tmpl->Execute(b, data);
        EXPECT_EQ(err, nullptr) << test.desc << ": template execution failed: " << str(err);
        EXPECT_EQ(b.str().str(), test.output) << test.desc;
    }
}

namespace {

struct dataItem {
    std::vector<std::shared_ptr<dataItem>> Children;
    string X;
};

std::shared_ptr<dataItem> item(string x, std::vector<std::shared_ptr<dataItem>> children = {}) {
    return std::make_shared<dataItem>(dataItem{std::move(children), std::move(x)});
}

}  // namespace

TEST(GoHTMLTemplate, EscapeSet) {
    dataItem data{{item("foo"), item("<bar>"), item("", {item("baz")})}, ""};

    struct {
        std::map<std::string, std::string> inputs;
        std::string want;
    } tests[] = {
        // The trivial set.
        {{{"main", ""}}, ""},
        // A template called in the start context.
        {{{"main", R"(Hello, {{template "helper"}}!)"},
          // Not a valid top level HTML template.
          // "<b" is not a full tag.
          {"helper", R"({{"<World>"}})"}},
         "Hello, &lt;World&gt;!"},
        // A template called in a context other than the start.
        {{{"main", R"(<a onclick='a = {{template "helper"}};'>)"},
          // Not a valid top level HTML template.
          // "<b" is not a full tag.
          {"helper", R"({{"<a>"}}<b)"}},
         R"(<a onclick='a = &#34;\u003ca\u003e&#34;<b;'>)"},
        // A recursive template that ends in its start context.
        {{{"main", R"({{range .Children}}{{template "main" .}}{{else}}{{.X}} {{end}})"}}, "foo &lt;bar&gt; baz "},
        // A recursive helper template that ends in its start context.
        {{{"main", R"({{template "helper" .}})"},
          {"helper",
           R"({{if .Children}}<ul>{{range .Children}}<li>{{template "main" .}}</li>{{end}}</ul>{{else}}{{.X}}{{end}})"}},
         "<ul><li>foo</li><li>&lt;bar&gt;</li><li><ul><li>baz</li></ul></li></ul>"},
        // Co-recursive templates that end in its start context.
        {{{"main", R"(<blockquote>{{range .Children}}{{template "helper" .}}{{end}}</blockquote>)"},
          {"helper", R"({{if .Children}}{{template "main" .}}{{else}}{{.X}}<br>{{end}})"}},
         "<blockquote>foo<br>&lt;bar&gt;<br><blockquote>baz<br></blockquote></blockquote>"},
        // A template that is called in two different contexts.
        {{{"main", R"(<button onclick="title='{{template "helper"}}'; ...">{{template "helper"}}</button>)"},
          {"helper", R"({{11}} of {{"<100>"}})"}},
         R"(<button onclick="title='11 of \u003c100\u003e'; ...">11 of &lt;100&gt;</button>)"},
        // A non-recursive template that ends in a different context.
        // helper starts in jsCtxRegexp and ends in jsCtxDivOp.
        {{{"main", R"(<script>var x={{template "helper"}}/{{"42"}};</script>)"}, {"helper", "{{126}}"}},
         R"(<script>var x= 126 /"42";</script>)"},
        // A recursive template that ends in a similar context.
        {{{"main", R"(<script>var x=[{{template "countdown" 4}}];</script>)"},
          {"countdown", R"({{.}}{{if .}},{{template "countdown" . | pred}}{{end}})"}},
         "<script>var x=[ 4 , 3 , 2 , 1 , 0 ];</script>"},
    };

    // pred is a template function that returns the predecessor of a
    // natural number for testing recursive templates.
    htmpl::FuncMap fns = {
        {"pred", ttmpl::Func::Raw(1, true, [](std::span<const Value> a) -> std::pair<Value, gopp::error> {
             if (a.size() == 1 && a[0].Kind() == ttmpl::Kind::Int && a[0].Int() > 0) {
                 return {Value::Of(int(a[0].Int() - 1)), nullptr};
             }
             return {Value(), errors::New("undefined pred")};
         })},
    };

    for (const auto& test : tests) {
        std::string source;
        for (const auto& [name, body] : test.inputs) source += "{{define \"" + name + "\"}}" + body + "{{end}} ";
        auto [tmpl, err] = htmpl::New("root")->Funcs(fns)->Parse(source);
        if (err) {
            ADD_FAILURE() << "error parsing " << source << ": " << str(err);
            continue;
        }
        strings::Builder b;
        if (auto xerr = tmpl->ExecuteTemplate(b, "main", data)) {
            ADD_FAILURE() << str(xerr) << " executing " << source;
            continue;
        }
        EXPECT_EQ(b.str().str(), test.want) << source;
    }
}

namespace {

struct errorsTest {
    std::string input;
    std::string err;
};

// clang-format off
const errorsTest errorsTests[] = {
    // Non-error cases.
    {"{{if .Cond}}<a>{{else}}<b>{{end}}", ""},
    {"{{if .Cond}}<a>{{end}}", ""},
    {"{{if .Cond}}{{else}}<b>{{end}}", ""},
    {"{{with .Cond}}<div>{{end}}", ""},
    {"{{range .Items}}<a>{{end}}", ""},
    {R"(<a href='/foo?{{range .Items}}&{{.K}}={{.V}}{{end}}'>)", ""},
    {"{{range .Items}}<a{{if .X}}{{end}}>{{end}}", ""},
    {"{{range .Items}}<a{{if .X}}{{end}}>{{continue}}{{end}}", ""},
    {"{{range .Items}}<a{{if .X}}{{end}}>{{break}}{{end}}", ""},
    {"{{range .Items}}<a{{if .X}}{{end}}>{{if .X}}{{break}}{{end}}{{end}}", ""},
    {"<script>var a = `${a+b}`</script>`", ""},
    {"<script>var tmpl = `asd`;</script>", ""},
    {"<script>var tmpl = `${1}`;</script>", ""},
    {"<script>var tmpl = `${return ``}`;</script>", ""},
    {"<script>var tmpl = `${return {{.}} }`;</script>", ""},
    {"<script>var tmpl = `${ let a = {1:1} {{.}} }`;</script>", ""},
    {R"(<script>var tmpl = `asd ${return "{"}`;</script>)", ""},
    {R"({{if eq "" ""}}<meta>{{end}})", ""},
    {R"({{if eq "" ""}}<meta content="url={{"asd"}}">{{end}})", ""},
    // Error cases.
    {"{{if .Cond}}<a{{end}}", "z:1:5: {{if}} branches"},
    {"{{if .Cond}}\n{{else}}\n<a{{end}}", "z:1:5: {{if}} branches"},
    {
        // Missing quote in the else branch.
        R"({{if .Cond}}<a href="foo">{{else}}<a href="bar>{{end}})",
        "z:1:5: {{if}} branches",
    },
    {
        // Different kind of attribute: href implies a URL.
        "<a {{if .Cond}}href='{{else}}title='{{end}}{{.X}}'>",
        "z:1:8: {{if}} branches",
    },
    {"\n{{with .X}}<a{{end}}", "z:2:7: {{with}} branches"},
    {"\n{{with .X}}<a>{{else}}<a{{end}}", "z:2:7: {{with}} branches"},
    {"{{range .Items}}<a{{end}}", R"(z:1: on range loop re-entry: "<" in attribute name: "<a")"},
    {"\n{{range .Items}} x='<a{{end}}", "z:2:8: on range loop re-entry: {{range}} branches"},
    {
        "{{range .Items}}<a{{if .X}}{{break}}{{end}}>{{end}}",
        "z:1:29: at range loop break: {{range}} branches end in different contexts",
    },
    {
        "{{range .Items}}<a{{if .X}}{{continue}}{{end}}>{{end}}",
        "z:1:29: at range loop continue: {{range}} branches end in different contexts",
    },
    {
        "{{range .Items}}{{if .X}}{{break}}{{end}}<a{{if .Y}}{{continue}}{{end}}>{{if .Z}}{{continue}}{{end}}{{end}}",
        "z:1:54: at range loop continue: {{range}} branches end in different contexts",
    },
    {"<a b=1 c={{.H}}", "z: ends in a non-text context: {stateAttr delimSpaceOrTagEnd"},
    {"<script>foo();", "z: ends in a non-text context: {stateJS"},
    {
        R"(<a href="{{if .F}}/foo?a={{else}}/bar/{{end}}{{.H}}">)",
        "z:1:47: {{.H}} appears in an ambiguous context within a URL",
    },
    {R"(<a onclick="alert('Hello \)", R"(unfinished escape sequence in JS string: "Hello \\")"},
    {
        R"(<a onclick='alert("Hello\, World\)",
        R"(unfinished escape sequence in JS string: "Hello\\, World\\")",
    },
    {R"(<a onclick='alert(/x+\)", R"(unfinished escape sequence in JS string: "x+\\")"},
    {R"(<a onclick="/foo[\]/)", R"(unfinished JS regexp charset: "foo[\\]/")"},
    {
        // It is ambiguous whether 1.5 should be 1\.5 or 1.5.
        // Either `var x = 1/- 1.5 /i.test(x)`
        // where `i.test(x)` is a method call of reference i,
        // or `/-1\.5/i.test(x)` which is a method call on a
        // case insensitive regular expression.
        R"(<script>{{if false}}var x = 1{{end}}/-{{"1.5"}}/i.test(x)</script>)",
        R"('/' could start a division or regexp: "/-")",
    },
    {R"({{template "foo"}})", R"(z:1:11: no such template "foo")"},
    {R"(<div{{template "y"}}>{{define "y"}} foo<b{{end}})", R"("<" in attribute name: " foo<b")"},
    {
        R"(<script>reverseList = [{{template "t"}}]</script>{{define "t"}}{{if .Tail}}{{template "t" .Tail}}{{end}}{{.Head}}",{{end}})",
        ": cannot compute output context for template t$htmltemplate_stateJS_elementScript",
    },
    {
        "<input type=button value=onclick=>",
        R"(html/template:z: "=" in unquoted attr: "onclick=")",
    },
    {
        "<input type=button value= onclick=>",
        R"(html/template:z: "=" in unquoted attr: "onclick=")",
    },
    {"<input type=button value= 1+1=2>", R"(html/template:z: "=" in unquoted attr: "1+1=2")"},
    {"<a class=`foo>", R"(html/template:z: "`" in unquoted attr: "`foo")"},
    {"<a style=font:'Arial'>", R"(html/template:z: "'" in unquoted attr: "font:'Arial'")"},
    {"<a=foo>", R"(: expected space, attr name, or end of tag, but got "=foo>")"},
    {
        "Hello, {{. | urlquery | print}}!",
        // urlquery is disallowed if it is not the last command in the pipeline.
        R"(predefined escaper "urlquery" disallowed in template)",
    },
    {
        "Hello, {{. | html | print}}!",
        // html is disallowed if it is not the last command in the pipeline.
        R"(predefined escaper "html" disallowed in template)",
    },
    {
        "Hello, {{html . | print}}!",
        // A direct call to html is disallowed if it is not the last command in the pipeline.
        R"(predefined escaper "html" disallowed in template)",
    },
    {
        "<div class={{. | html}}>Hello<div>",
        // html is disallowed in a pipeline that is in an unquoted attribute context,
        // even if it is the last command in the pipeline.
        R"(predefined escaper "html" disallowed in template)",
    },
    {
        "Hello, {{. | urlquery | html}}!",
        // html is allowed since it is the last command in the pipeline, but urlquery is not.
        R"(predefined escaper "urlquery" disallowed in template)",
    },
    {"<script>var a = `{{if .X}}`{{end}}", "{{if}} branches end in different contexts"},
    {"<script>var a = `{{if .X}}a{{else}}`{{end}}", "{{if}} branches end in different contexts"},
    {"<script>var a = `{{if .X}}a{{else}}b{{end}}`</script>", ""},
};
// clang-format on

}  // namespace

TEST(GoHTMLTemplate, Errors) {
    for (const auto& test : errorsTests) {
        SCOPED_TRACE(test.input);
        bytes::Buffer buf;
        auto [tmpl, err] = htmpl::New("z")->Parse(test.input);
        if (err) {
            ADD_FAILURE() << "unexpected parse error " << str(err);
            continue;
        }
        err = tmpl->Execute(buf, nullptr);
        std::string got = str(err);
        if (test.err.empty()) {
            EXPECT_EQ(got, "") << "unexpected error";
            continue;
        }
        if (got.find(test.err) == std::string::npos) {
            ADD_FAILURE() << "error\n\t" << got << "\ndoes not contain expected string\n\t" << test.err;
            continue;
        }
        // Check that we get the same error if we call Execute again.
        auto err2 = tmpl->Execute(buf, nullptr);
        EXPECT_TRUE(err2 && str(err2) == got) << "unexpected error on second call " << str(err2);
    }
}

namespace {

// A context, built like Go's composite literals.
struct C {
    context c;
    C& st(state s) {
        c.st = s;
        return *this;
    }
    C& dl(delim d) {
        c.dl = d;
        return *this;
    }
    C& up(urlPart u) {
        c.up = u;
        return *this;
    }
    C& js(jsCtx j) {
        c.js = j;
        return *this;
    }
    C& at(attr a) {
        c.at = a;
        return *this;
    }
    C& el(element e) {
        c.el = e;
        return *this;
    }
    C& depth(std::vector<int> d) {
        c.jsBraceDepth = std::move(d);
        c.hasJSBraceDepth = true;
        return *this;
    }
};

struct escapeTextTest {
    std::string input;
    C output;
};

}  // namespace

TEST(GoHTMLTemplate, EscapeText) {
    // clang-format off
    const escapeTextTest tests[] = {
        {"", C()},
        {"Hello, World!", C()},
        {
            // An orphaned "<" is OK.
            "I <3 Ponies!",
            C(),
        },
        {"<a", C().st(stateTag)},
        {"<a ", C().st(stateTag)},
        {"<a>", C().st(stateText)},
        {"<a href", C().st(stateAttrName).at(attrURL)},
        {"<a on", C().st(stateAttrName).at(attrScript)},
        {"<a href ", C().st(stateAfterName).at(attrURL)},
        {"<a style  =  ", C().st(stateBeforeValue).at(attrStyle)},
        {"<a href=", C().st(stateBeforeValue).at(attrURL)},
        {"<a href=x", C().st(stateURL).dl(delimSpaceOrTagEnd).up(urlPartPreQuery).at(attrURL)},
        {"<a href=x ", C().st(stateTag)},
        {"<a href=>", C().st(stateText)},
        {"<a href=x>", C().st(stateText)},
        {"<a href ='", C().st(stateURL).dl(delimSingleQuote).at(attrURL)},
        {"<a href=''", C().st(stateTag)},
        {R"(<a href= ")", C().st(stateURL).dl(delimDoubleQuote).at(attrURL)},
        {R"(<a href="")", C().st(stateTag)},
        {R"(<a title=")", C().st(stateAttr).dl(delimDoubleQuote)},
        {"<a HREF='http:", C().st(stateURL).dl(delimSingleQuote).up(urlPartPreQuery).at(attrURL)},
        {"<a Href='/", C().st(stateURL).dl(delimSingleQuote).up(urlPartPreQuery).at(attrURL)},
        {R"(<a href='")", C().st(stateURL).dl(delimSingleQuote).up(urlPartPreQuery).at(attrURL)},
        {R"(<a href="')", C().st(stateURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrURL)},
        {"<a href='&apos;", C().st(stateURL).dl(delimSingleQuote).up(urlPartPreQuery).at(attrURL)},
        {R"(<a href="&quot;)", C().st(stateURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrURL)},
        {R"(<a href="&#34;)", C().st(stateURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrURL)},
        {"<a href=&quot;", C().st(stateURL).dl(delimSpaceOrTagEnd).up(urlPartPreQuery).at(attrURL)},
        {R"(<img alt="1">)", C().st(stateText)},
        {R"(<img alt="1>")", C().st(stateTag)},
        {R"(<img alt="1>">)", C().st(stateText)},
        {R"(<input checked type="checkbox")", C().st(stateTag)},
        {R"(<a onclick=")", C().st(stateJS).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="//foo)", C().st(stateJSLineCmt).dl(delimDoubleQuote).at(attrScript)},
        {"<a onclick='//\n", C().st(stateJS).dl(delimSingleQuote).at(attrScript)},
        {"<a onclick='//\r\n", C().st(stateJS).dl(delimSingleQuote).at(attrScript)},
        {"<a onclick='// ", C().st(stateJS).dl(delimSingleQuote).at(attrScript)},
        {R"(<a onclick="/*)", C().st(stateJSBlockCmt).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/*/)", C().st(stateJSBlockCmt).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/**/)", C().st(stateJS).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onkeypress="&quot;)", C().st(stateJSDqStr).dl(delimDoubleQuote).at(attrScript)},
        {
            "<a onclick='&quot;foo&quot;",
            C().st(stateJS).dl(delimSingleQuote).js(jsCtxDivOp).at(attrScript),
        },
        {
            "<a onclick=&#39;foo&#39;",
            C().st(stateJS).dl(delimSpaceOrTagEnd).js(jsCtxDivOp).at(attrScript),
        },
        {"<a onclick=&#39;foo", C().st(stateJSSqStr).dl(delimSpaceOrTagEnd).at(attrScript)},
        {R"(<a onclick="&quot;foo')", C().st(stateJSDqStr).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="'foo&quot;)", C().st(stateJSSqStr).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="`foo)", C().st(stateJSTmplLit).dl(delimDoubleQuote).at(attrScript)},
        {R"(<A ONCLICK="')", C().st(stateJSSqStr).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/)", C().st(stateJSRegexp).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="'foo')", C().st(stateJS).dl(delimDoubleQuote).js(jsCtxDivOp).at(attrScript)},
        {R"(<a onclick="'foo\')", C().st(stateJSSqStr).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="'foo\')", C().st(stateJSSqStr).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/foo/)", C().st(stateJS).dl(delimDoubleQuote).js(jsCtxDivOp).at(attrScript)},
        {"<script>/foo/ /=", C().st(stateJS).el(elementScript)},
        {R"(<a onclick="1 /foo)", C().st(stateJS).dl(delimDoubleQuote).js(jsCtxDivOp).at(attrScript)},
        {
            R"(<a onclick="1 /*c*/ /foo)",
            C().st(stateJS).dl(delimDoubleQuote).js(jsCtxDivOp).at(attrScript),
        },
        {R"(<a onclick="/foo[/])", C().st(stateJSRegexp).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/foo\/)", C().st(stateJSRegexp).dl(delimDoubleQuote).at(attrScript)},
        {R"(<a onclick="/foo/)", C().st(stateJS).dl(delimDoubleQuote).js(jsCtxDivOp).at(attrScript)},
        {R"(<input checked style=")", C().st(stateCSS).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="//)", C().st(stateCSSLineCmt).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="//</script>)", C().st(stateCSSLineCmt).dl(delimDoubleQuote).at(attrStyle)},
        {"<a style='//\n", C().st(stateCSS).dl(delimSingleQuote).at(attrStyle)},
        {"<a style='//\r", C().st(stateCSS).dl(delimSingleQuote).at(attrStyle)},
        {R"(<a style="/*)", C().st(stateCSSBlockCmt).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="/*/)", C().st(stateCSSBlockCmt).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="/**/)", C().st(stateCSS).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="background: ')", C().st(stateCSSSqStr).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="background: &quot;)", C().st(stateCSSDqStr).dl(delimDoubleQuote).at(attrStyle)},
        {
            R"(<a style="background: '/foo?img=)",
            C().st(stateCSSSqStr).dl(delimDoubleQuote).up(urlPartQueryOrFrag).at(attrStyle),
        },
        {
            R"(<a style="background: '/)",
            C().st(stateCSSSqStr).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {
            R"(<a style="background: url(&#x22;/)",
            C().st(stateCSSDqURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {
            R"(<a style="background: url('/)",
            C().st(stateCSSSqURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {
            R"(<a style="background: url('/))",
            C().st(stateCSSSqURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {
            R"(<a style="background: url('/ )",
            C().st(stateCSSSqURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {
            R"(<a style="background: url(/)",
            C().st(stateCSSURL).dl(delimDoubleQuote).up(urlPartPreQuery).at(attrStyle),
        },
        {R"(<a style="background: url( )", C().st(stateCSSURL).dl(delimDoubleQuote).at(attrStyle)},
        {
            R"(<a style="background: url( /image?name=)",
            C().st(stateCSSURL).dl(delimDoubleQuote).up(urlPartQueryOrFrag).at(attrStyle),
        },
        {R"(<a style="background: url(x))", C().st(stateCSS).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="background: url('x')", C().st(stateCSS).dl(delimDoubleQuote).at(attrStyle)},
        {R"(<a style="background: url( x )", C().st(stateCSS).dl(delimDoubleQuote).at(attrStyle)},
        {"<!-- foo", C().st(stateHTMLCmt)},
        {"<!-->", C().st(stateHTMLCmt)},
        {"<!--->", C().st(stateHTMLCmt)},
        {"<!-- foo -->", C().st(stateText)},
        {"<script", C().st(stateTag).el(elementScript)},
        {"<script ", C().st(stateTag).el(elementScript)},
        {R"(<script src="foo.js" )", C().st(stateTag).el(elementScript)},
        {"<script src='foo.js' ", C().st(stateTag).el(elementScript)},
        {"<script type=text/javascript ", C().st(stateTag).el(elementScript)},
        {"<script>", C().st(stateJS).js(jsCtxRegexp).el(elementScript)},
        {"<script>foo", C().st(stateJS).js(jsCtxDivOp).el(elementScript)},
        {"<script>foo</script>", C().st(stateText)},
        {"<script>foo</script><!--", C().st(stateHTMLCmt)},
        {R"(<script>document.write("<p>foo</p>");)", C().st(stateJS).el(elementScript)},
        {R"(<script>document.write("<p>foo<\/script>");)", C().st(stateJS).el(elementScript)},
        {
            // <script and </script tags are escaped, so </script> should not
            // cause us to exit the JS state.
            R"(<script>document.write("<script>alert(1)</script>");)",
            C().st(stateJS).el(elementScript),
        },
        {R"(<script>document.write("<script>)", C().st(stateJSDqStr).el(elementScript)},
        {
            R"(<script>document.write("<script>alert(1)</script>)",
            C().st(stateJSDqStr).el(elementScript),
        },
        {R"(<script>document.write("<script>alert(1)<!--)", C().st(stateJSDqStr).el(elementScript)},
        {
            R"(<script>document.write("<script>alert(1)</Script>");)",
            C().st(stateJS).el(elementScript),
        },
        {R"(<script>document.write("<!--");)", C().st(stateJS).el(elementScript)},
        {"<script>let a = /</script", C().st(stateJSRegexp).el(elementScript)},
        {"<script>let a = /</script/", C().st(stateJS).el(elementScript).js(jsCtxDivOp)},
        {R"(<script type="text/template">)", C().st(stateText)},
        // covering issue 19968
        {R"(<script type="TEXT/JAVASCRIPT">)", C().st(stateJS).el(elementScript)},
        // covering issue 19965
        {R"(<script TYPE="text/template">)", C().st(stateText)},
        {R"(<script type="notjs">)", C().st(stateText)},
        {"<Script>", C().st(stateJS).el(elementScript)},
        {"<SCRIPT>foo", C().st(stateJS).js(jsCtxDivOp).el(elementScript)},
        {"<textarea>value", C().st(stateRCDATA).el(elementTextarea)},
        {"<textarea>value</TEXTAREA>", C().st(stateText)},
        {"<textarea name=html><b", C().st(stateRCDATA).el(elementTextarea)},
        {"<title>value", C().st(stateRCDATA).el(elementTitle)},
        {"<style>value", C().st(stateCSS).el(elementStyle)},
        {"<a xlink:href", C().st(stateAttrName).at(attrURL)},
        {"<a xmlns", C().st(stateAttrName).at(attrURL)},
        {"<a xmlns:foo", C().st(stateAttrName).at(attrURL)},
        {"<a xmlnsxyz", C().st(stateAttrName)},
        {"<a data-url", C().st(stateAttrName).at(attrURL)},
        {"<a data-iconUri", C().st(stateAttrName).at(attrURL)},
        {"<a data-urlItem", C().st(stateAttrName).at(attrURL)},
        {"<a g:", C().st(stateAttrName)},
        {"<a g:url", C().st(stateAttrName).at(attrURL)},
        {"<a g:iconUri", C().st(stateAttrName).at(attrURL)},
        {"<a g:urlItem", C().st(stateAttrName).at(attrURL)},
        {"<a g:value", C().st(stateAttrName)},
        {"<a svg:style='", C().st(stateCSS).dl(delimSingleQuote).at(attrStyle)},
        {"<svg:font-face", C().st(stateTag)},
        {R"(<svg:a svg:onclick=")", C().st(stateJS).dl(delimDoubleQuote).at(attrScript)},
        {"<svg:a svg:onclick=\"x()\">", C()},
        {"<script>var a = `", C().st(stateJSTmplLit).el(elementScript)},
        {"<script>var a = `${", C().st(stateJS).el(elementScript).depth({0})},
        {"<script>var a = `${}", C().st(stateJSTmplLit).el(elementScript)},
        {"<script>var a = `${`", C().st(stateJSTmplLit).el(elementScript).depth({0})},
        {R"(<script>var a = `${var a = ")", C().st(stateJSDqStr).el(elementScript).depth({0})},
        {R"(<script>var a = `${var a = "`)", C().st(stateJSDqStr).el(elementScript).depth({0})},
        {R"(<script>var a = `${var a = "})", C().st(stateJSDqStr).el(elementScript).depth({0})},
        {"<script>var a = `${``", C().st(stateJS).el(elementScript).depth({0})},
        {"<script>var a = `${`}", C().st(stateJSTmplLit).el(elementScript).depth({0})},
        {"<script>`${ {} } asd`</script><script>`${ {} }", C().st(stateJSTmplLit).el(elementScript)},
        {
            R"(<script>var foo = `${ (_ => { return "x" })() + "${)",
            C().st(stateJSDqStr).el(elementScript).depth({0}),
        },
        {
            "<script>var a = `${ {</script><script>var b = `${ x }",
            C().st(stateJSTmplLit).el(elementScript).js(jsCtxDivOp),
        },
        {R"(<script>var foo = `x` + "${)", C().st(stateJSDqStr).el(elementScript)},
        {"<script>function f() { var a = `${}`; }", C().st(stateJS).el(elementScript)},
        {"<script>{`${}`}", C().st(stateJS).el(elementScript)},
        {
            "<script>`${ function f() { return `${1}` }() }`",
            C().st(stateJS).el(elementScript).js(jsCtxDivOp),
        },
        {
            "<script>function f() {`${ function f() { `${1}` } }`}",
            C().st(stateJS).el(elementScript).js(jsCtxRegexp),
        },
        {"<script>`${ { `` }", C().st(stateJS).el(elementScript).depth({0})},
        {"<script>`${ { }`", C().st(stateJSTmplLit).el(elementScript).depth({0})},
        {"<script>var foo = `${ foo({ a: { c: `${", C().st(stateJS).el(elementScript).depth({2, 0})},
        {
            "<script>var foo = `${ foo({ a: { c: `${ {{.}} }` }, b: ",
            C().st(stateJS).el(elementScript).depth({1}),
        },
        {"<script>`${ `}", C().st(stateJSTmplLit).el(elementScript).depth({0})},
        {"<script>`${1}${", C().st(stateJS).el(elementScript).js(jsCtxRegexp).depth({0})},
        {"<script>`${`${1}${", C().st(stateJS).el(elementScript).js(jsCtxRegexp).depth({0, 0})},
    };
    // clang-format on

    for (const auto& test : tests) {
        SCOPED_TRACE(test.input);
        auto node = make_ptr<parse::TextNode>();
        node->NodeType = parse::NodeText;
        node->Text = string(test.input);
        context c = escapeTextNode(context{}, node.get());
        EXPECT_TRUE(test.output.c.eq(c)) << "want context\n\t" << test.output.c.String() << "\ngot\n\t" << c.String();
        EXPECT_EQ(node->Text, test.input) << "text node was modified";
    }
}

TEST(GoHTMLTemplate, EnsurePipelineContains) {
    struct {
        std::string input, output;
        std::vector<std::string> ids;
    } tests[] = {
        {"{{.X}}", ".X", {}},
        {"{{.X | html}}", ".X | html", {}},
        {"{{.X}}", ".X | html", {"html"}},
        {"{{html .X}}", "_eval_args_ .X | html | urlquery", {"html", "urlquery"}},
        {"{{html .X .Y .Z}}", "_eval_args_ .X .Y .Z | html | urlquery", {"html", "urlquery"}},
        {"{{.X | print}}", ".X | print | urlquery", {"urlquery"}},
        {"{{.X | print | urlquery}}", ".X | print | urlquery", {"urlquery"}},
        {"{{.X | urlquery}}", ".X | html | urlquery", {"html", "urlquery"}},
        {"{{.X | print 2 | .f 3}}", ".X | print 2 | .f 3 | urlquery | html", {"urlquery", "html"}},
        {
            // covering issue 10801
            "{{.X | println.x }}",
            ".X | println.x | urlquery | html",
            {"urlquery", "html"},
        },
        {
            // covering issue 10801
            "{{.X | (print 12 | println).x }}",
            ".X | (print 12 | println).x | urlquery | html",
            {"urlquery", "html"},
        },
        // The following test cases ensure that the merging of internal escapers
        // with the predefined "html" and "urlquery" escapers is correct.
        {
            "{{.X | urlquery}}",
            ".X | _html_template_urlfilter | urlquery",
            {"_html_template_urlfilter", "_html_template_urlnormalizer"},
        },
        {
            "{{.X | urlquery}}",
            ".X | urlquery | _html_template_urlfilter | _html_template_cssescaper",
            {"_html_template_urlfilter", "_html_template_cssescaper"},
        },
        {"{{.X | urlquery}}", ".X | urlquery", {"_html_template_urlnormalizer"}},
        {"{{.X | urlquery}}", ".X | urlquery", {"_html_template_urlescaper"}},
        {"{{.X | html}}", ".X | html", {"_html_template_htmlescaper"}},
        {"{{.X | html}}", ".X | html", {"_html_template_rcdataescaper"}},
    };
    for (std::size_t i = 0; i < std::size(tests); i++) {
        const auto& test = tests[i];
        auto tmpl = ttmpl::Must(ttmpl::New("test")->Parse(test.input));
        const auto& first = tmpl->Tree->Root->Nodes[0];
        if (first->NodeType != parse::NodeAction) {
            ADD_FAILURE() << "First node is not an action: " << test.input;
            continue;
        }
        auto* pipe = static_cast<parse::ActionNode*>(first.get())->Pipe.get();
        ensurePipelineContains(pipe, test.ids);
        EXPECT_EQ(pipe->String(), test.output) << "#" << i << ": " << test.input;
    }
}

TEST(GoHTMLTemplate, EscapeMalformedPipelines) {
    const char* tests[] = {
        "{{ 0 | $ }}",
        "{{ 0 | $ | urlquery }}",
        "{{ 0 | (nil) }}",
        "{{ 0 | (nil) | html }}",
    };
    for (const char* test : tests) {
        bytes::Buffer b;
        auto [tmpl, err] = htmpl::New("test")->Parse(test);
        if (err) {
            ADD_FAILURE() << "failed to parse set: " << str(err);
            continue;
        }
        EXPECT_NE(tmpl->Execute(b, nullptr), nullptr) << "Expected error for " << test;
    }
}

TEST(GoHTMLTemplate, EscapeErrorsNotIgnorable) {
    bytes::Buffer b;
    auto tmpl = htmpl::New("dangerous")->Parse("<a").first;
    auto err = tmpl->Execute(b, nullptr);
    EXPECT_NE(err, nullptr) << "Expected error";
    EXPECT_EQ(b.Len(), 0) << "Emitted output despite escaping failure";
}

TEST(GoHTMLTemplate, EscapeSetErrorsNotIgnorable) {
    bytes::Buffer b;
    auto [tmpl, err] = htmpl::New("root")->Parse(R"({{define "t"}}<a{{end}})");
    ASSERT_EQ(err, nullptr) << "failed to parse set: " << str(err);
    err = tmpl->ExecuteTemplate(b, "t", nullptr);
    EXPECT_NE(err, nullptr) << "Expected error";
    EXPECT_EQ(b.Len(), 0) << "Emitted output despite escaping failure";
}

TEST(GoHTMLTemplate, RedundantFuncs) {
    std::vector<Value> inputs = {
        Value::Of(std::string("\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f", 16) +
                  "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f" +
                  R"( !"#$%&'()*+,-./)" + "0123456789:;<=>?" + "@ABCDEFGHIJKLMNO" + R"(PQRSTUVWXYZ[\]^_)" +
                  "`abcdefghijklmno" + "pqrstuvwxyz{|}~\x7f" +
                  " Ā  ﻿﷬�￿\U0001D11E" + "&amp;%22\\"),
        Value::Of(htmpl::CSS(R"(a[href =~ "//example.com"]#foo)")),
        Value::Of(htmpl::HTML("Hello, <b>World</b> &amp;tc!")),
        Value::Of(htmpl::HTMLAttr(R"( dir="ltr")")),
        Value::Of(htmpl::JS(R"(c && alert("Hello, World!");)")),
        Value::Of(htmpl::JSStr(R"(Hello, World & O'Reilly\x21)")),
        Value::Of(htmpl::URL("greeting=H%69&addressee=(World)")),
    };

    const auto& fm = funcMap();
    for (const auto& [n0, f0] : fm) {
        for (const auto& [n1, f1] : fm) {
            if (!redundantFunc(n0, n1)) continue;
            for (const auto& input : inputs) {
                Value want = f0.Call(std::span(&input, 1)).first;
                Value got = f1.Call(std::span(&want, 1)).first;
                EXPECT_EQ(got.Str().str(), want.Str().str()) << n0 << " " << n1 << " with " << input.TypeName() << " " << input.Str();
            }
        }
    }
}

TEST(GoHTMLTemplate, IndirectPrint) {
    int a = 3;
    int* ap = &a;
    std::string b = "hello";
    std::string* bp = &b;
    std::string** bpp = &bp;
    auto tmpl = htmpl::Must(htmpl::New("t")->Parse("{{.}}"));
    strings::Builder buf;
    auto err = tmpl->Execute(buf, ap);
    EXPECT_EQ(err, nullptr) << "Unexpected error: " << str(err);
    EXPECT_EQ(buf.str().str(), "3");
    strings::Builder buf2;
    err = tmpl->Execute(buf2, bpp);
    EXPECT_EQ(err, nullptr) << "Unexpected error: " << str(err);
    EXPECT_EQ(buf2.str().str(), "hello");
}

// This is a test for issue 3272.
TEST(GoHTMLTemplate, EmptyTemplateHTML) {
    auto page = htmpl::Must(htmpl::New("page")->ParseFiles({"/dev/null"}));
    bytes::Buffer out;
    EXPECT_NE(page->ExecuteTemplate(out, "page", std::string("nothing")), nullptr) << "expected error";
}

namespace {

struct Issue7379 {
    int v = 0;
    string SomeMethod(int x) const { return "<" + std::to_string(x) + ">"; }
};

}  // namespace

// This is a test for issue 7379: type assertion error caused panic, and
// then the code to handle the panic breaks escaping.
TEST(GoHTMLTemplate, PipeToMethodIsEscaped) {
    auto tmpl = htmpl::Must(htmpl::New("x")->Parse("<html>{{0 | .SomeMethod}}</html>\n"));
    for (int i = 0; i < 3; i++) {
        strings::Builder b;
        (void)tmpl->Execute(b, Issue7379{});
        EXPECT_EQ(b.str().str(), "<html>&lt;0&gt;</html>\n");
    }
}

// Unlike text/template, html/template crashed if given an incomplete
// template, that is, a template that had been named but not given any
// content. This is issue #10204.
TEST(GoHTMLTemplate, ErrorOnUndefined) {
    auto tmpl = htmpl::New("undefined");
    bytes::Buffer b;
    auto err = tmpl->Execute(b, nullptr);
    ASSERT_NE(err, nullptr) << "expected error";
    EXPECT_NE(str(err).find("incomplete"), std::string::npos) << "expected error about incomplete template; got " << str(err);
}

// This covers issue #20842.
TEST(GoHTMLTemplate, IdempotentExecute) {
    auto tmpl = htmpl::Must(htmpl::New("")->Parse(R"({{define "main"}}<body>{{template "hello"}}</body>{{end}})"));
    htmpl::Must(tmpl->Parse(R"({{define "hello"}}Hello, {{"Ladies & Gentlemen!"}}{{end}})"));
    // Ensure that "hello" produces the same output when executed twice.
    std::string want = "Hello, Ladies &amp; Gentlemen!";
    for (int i = 0; i < 2; i++) {
        strings::Builder got;
        auto err = tmpl->ExecuteTemplate(got, "hello", nullptr);
        EXPECT_EQ(err, nullptr) << "unexpected error: " << str(err);
        EXPECT_EQ(got.str().str(), want) << "after executing template \"hello\"";
    }
    // Ensure that the implicit re-execution of "hello" during the execution
    // of "main" does not cause the output of "hello" to change.
    strings::Builder got;
    auto err = tmpl->ExecuteTemplate(got, "main", nullptr);
    EXPECT_EQ(err, nullptr) << "unexpected error: " << str(err);
    // If the HTML escaper is added again to the action
    // {{"Ladies & Gentlemen!"}}, we would expected to see the ampersand
    // overescaped to "&amp;amp;".
    EXPECT_EQ(got.str().str(), "<body>Hello, Ladies &amp; Gentlemen!</body>") << "after executing template \"main\"";
}

// Covers issue 22780.
TEST(GoHTMLTemplate, OrphanedTemplate) {
    auto t1 = htmpl::Must(htmpl::New("foo")->Parse(R"(<a href="{{.}}">link1</a>)"));
    auto t2 = htmpl::Must(t1->New("foo")->Parse("bar"));

    strings::Builder b;
    auto err = t1->Execute(b, std::string("javascript:alert(1)"));
    ASSERT_NE(err, nullptr) << "expected error executing t1";
    EXPECT_EQ(str(err), R"(template: "foo" is an incomplete or empty template)");
    strings::Builder b2;
    err = t2->Execute(b2, nullptr);
    ASSERT_EQ(err, nullptr) << "error executing t2: " << str(err);
    EXPECT_EQ(b2.str().str(), "bar");
}

// Covers issue 21844.
TEST(GoHTMLTemplate, AliasedParseTreeDoesNotOverescape) {
    const std::string data = "<baz>";
    const std::string want = "&lt;baz&gt;";
    // Templates "foo" and "bar" both alias the same underlying parse tree.
    auto tpl = htmpl::Must(htmpl::New("foo")->Parse("{{.}}"));
    auto [_, err] = tpl->AddParseTree("bar", tpl->Tree);
    ASSERT_EQ(err, nullptr) << "AddParseTree error: " << str(err);
    strings::Builder b1, b2;
    err = tpl->ExecuteTemplate(b1, "foo", data);
    ASSERT_EQ(err, nullptr) << "ExecuteTemplate failed for \"foo\": " << str(err);
    err = tpl->ExecuteTemplate(b2, "bar", data);
    ASSERT_EQ(err, nullptr) << "ExecuteTemplate failed for \"bar\": " << str(err);
    EXPECT_EQ(b1.str().str(), want);
    EXPECT_EQ(b1.str().str(), b2.str());
}

TEST(GoHTMLTemplate, Issue81821) {
    auto tmpl = htmpl::Must(htmpl::New("test")->Parse("<script>const s = `${1}${/{{.}}/g}`</script>"));
    bytes::Buffer buf;
    auto err = tmpl->Execute(buf, std::string("x/.exec(alert(1))}"));
    ASSERT_EQ(err, nullptr) << "Execute: " << str(err);
    EXPECT_EQ(asStringView(buf.Bytes()), "<script>const s = `${1}${/x\\/\\.exec\\(alert\\(1\\)\\)\\}/g}`</script>");
}

namespace {

struct execTest {
    std::string name, tmpl, input, want;
};

void runExecTests(std::span<const execTest> tests) {
    for (const auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        auto tmpl = htmpl::Must(htmpl::New("test")->Parse(tt.tmpl));
        bytes::Buffer buf;
        auto err = tmpl->Execute(buf, tt.input);
        ASSERT_EQ(err, nullptr) << "Execute: " << str(err);
        EXPECT_EQ(asStringView(buf.Bytes()), tt.want);
    }
}

}  // namespace

TEST(GoHTMLTemplate, CVE202656858) {
    const execTest tests[] = {
        {
            .name = "regexp after open brace in if block",
            .tmpl = R"(<script>if(true){/{{.}}/g.test("x")}</script>)",
            .input = "a.b",
            .want = R"(<script>if(true){/a\.b/g.test("x")}</script>)",
        },
        {
            .name = "regexp after close brace",
            .tmpl = R"(<script>if(true){x=1}/{{.}}/g.test("x")</script>)",
            .input = "a.b",
            .want = R"(<script>if(true){x=1}/a\.b/g.test("x")</script>)",
        },
        {
            .name = "regexp pathological attacker input",
            .tmpl = R"(<script>if(true){/{{.}}/g.test("x")}</script>)",
            .input = "./;alert(1);var q=/.",
            .want = R"(<script>if(true){/\.\/;alert\(1\);var q=\/\./g.test("x")}</script>)",
        },
        {
            .name = "regexp after open brace in template literal",
            .tmpl = "<script>`${ (function(){/{{.}}/g.test(x)}) }`</script>",
            .input = "a.b",
            .want = R"(<script>`${ (function(){/a\.b/g.test(x)}) }`</script>)",
        },
        {
            .name = "regexp after close brace in template literal",
            .tmpl = "<script>`${ (function(){}/{{.}}/g.test(x)) }`</script>",
            .input = "a.b",
            .want = R"(<script>`${ (function(){}/a\.b/g.test(x)) }`</script>)",
        },
    };
    runExecTests(tests);
}

TEST(GoHTMLTemplate, Issue81823) {
    const execTest tests[] = {
        {
            .name = "yield",
            .tmpl = "<script>function* rules(){yield /{{.}}/g}</script>",
            .input = "/;alert(1)//",
            .want = R"(<script>function* rules(){yield /\/;alert\(1\)\/\//g}</script>)",
        },
        {
            .name = "property yield",
            .tmpl = "<script>const pct = bond.yield / 100;\nconst user = {{.}}; // state\n</script>",
            .input = "1;pwned=1;0",
            .want = "<script>const pct = bond.yield / 100;\nconst user = \"1;pwned=1;0\"; \n</script>",
        },
        {
            .name = "private yield",
            .tmpl = "<script>class Bond { #yield = 4; pct() { return this.#yield / 100; } }\nconst user = {{.}}; // state\n</script>",
            .input = "1;pwned=1;0",
            .want = "<script>class Bond { #yield = 4; pct() { return this.#yield / 100; } }\nconst user = \"1;pwned=1;0\"; \n</script>",
        },
        {
            .name = "property in",
            .tmpl = "<script>const kb = traffic.in / 1024;\nconst user = {{.}}; // state\n</script>",
            .input = "1;pwned=1;0",
            .want = "<script>const kb = traffic.in / 1024;\nconst user = \"1;pwned=1;0\"; \n</script>",
        },
    };
    runExecTests(tests);
}
