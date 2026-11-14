// Ported from Go 1.27.3's html/template/content_test.go, clone_test.go,
// multi_test.go and template_test.go, with Go's test names. Go's
// TestParseFS and TestParseZipFS are left out (no io/fs), and so is
// TestTemplateLookUp, which Go skips. The multi tests run with the part of
// exec_test.go's data they use.
#include <gopp/bytes.hpp>
#include <gopp/chan.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/html/template.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace gopp;
namespace htmpl = gopp::html::tmpl;
namespace ttmpl = gopp::text::tmpl;
namespace parse = gopp::text::tmpl::parse;
using htmpl::Value;

namespace {

std::string str(const gopp::error& err) { return err ? err.Error().str() : "<nil>"; }

struct discard final : io::Writer {
    std::pair<isize, gopp::error> Write(std::span<const byte> p) override { return {isize(p.size()), nullptr}; }
};

}  // namespace

// ---------------------------------------------------------------------------
// content_test.go

TEST(GoHTMLTemplate, TypedContent) {
    std::vector<Value> data = {
        Value::Of(std::string(R"(<b> "foo%" O'Reilly &bar;)")),
        Value::Of(htmpl::CSS(R"(a[href =~ "//example.com"]#foo)")),
        Value::Of(htmpl::HTML("Hello, <b>World</b> &amp;tc!")),
        Value::Of(htmpl::HTMLAttr(R"( dir="ltr")")),
        Value::Of(htmpl::JS(R"(c && alert("Hello, World!");)")),
        Value::Of(htmpl::JSStr(R"(Hello, World & O'Reilly\u0021)")),
        Value::Of(htmpl::URL("greeting=H%69,&addressee=(World)")),
        Value::Of(htmpl::Srcset("greeting=H%69,&addressee=(World) 2x, https://golang.org/favicon.ico 500.5w")),
        Value::Of(htmpl::URL(",foo/,")),
    };

    // For each content sensitive escaper, see how it does on each of the
    // typed strings above.
    struct {
        // A template containing a single {{.}}.
        std::string input;
        std::vector<std::string> want;
    } tests[] = {
        {
            "<style>{{.}} { color: blue }</style>",
            {"ZgotmplZ", R"(a[href =~ "//example.com"]#foo)", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ"},
        },
        {
            R"(<div style="{{.}}">)",
            {"ZgotmplZ", "a[href =~ &#34;//example.com&#34;]#foo", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ"},
        },
        {
            "{{.}}",
            {"&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;", "a[href =~ &#34;//example.com&#34;]#foo", "Hello, <b>World</b> &amp;tc!", " dir=&#34;ltr&#34;", "c &amp;&amp; alert(&#34;Hello, World!&#34;);", R"(Hello, World &amp; O&#39;Reilly\u0021)", "greeting=H%69,&amp;addressee=(World)", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", ",foo/,"},
        },
        {
            "<a{{.}}>",
            {"ZgotmplZ", "ZgotmplZ", "ZgotmplZ", R"( dir="ltr")", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ", "ZgotmplZ"},
        },
        {
            "<a title={{.}}>",
            {"&lt;b&gt;&#32;&#34;foo%&#34;&#32;O&#39;Reilly&#32;&amp;bar;", "a[href&#32;&#61;~&#32;&#34;//example.com&#34;]#foo", "Hello,&#32;World&#32;&amp;tc!", "&#32;dir&#61;&#34;ltr&#34;", "c&#32;&amp;&amp;&#32;alert(&#34;Hello,&#32;World!&#34;);", R"(Hello,&#32;World&#32;&amp;&#32;O&#39;Reilly\u0021)", "greeting&#61;H%69,&amp;addressee&#61;(World)", "greeting&#61;H%69,&amp;addressee&#61;(World)&#32;2x,&#32;https://golang.org/favicon.ico&#32;500.5w", ",foo/,"},
        },
        {
            "<a title='{{.}}'>",
            {"&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;", "a[href =~ &#34;//example.com&#34;]#foo", "Hello, World &amp;tc!", " dir=&#34;ltr&#34;", "c &amp;&amp; alert(&#34;Hello, World!&#34;);", R"(Hello, World &amp; O&#39;Reilly\u0021)", "greeting=H%69,&amp;addressee=(World)", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", ",foo/,"},
        },
        {
            "<textarea>{{.}}</textarea>",
            {"&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;", "a[href =~ &#34;//example.com&#34;]#foo", "Hello, &lt;b&gt;World&lt;/b&gt; &amp;tc!", " dir=&#34;ltr&#34;", "c &amp;&amp; alert(&#34;Hello, World!&#34;);", R"(Hello, World &amp; O&#39;Reilly\u0021)", "greeting=H%69,&amp;addressee=(World)", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", ",foo/,"},
        },
        {
            "<script>alert({{.}})</script>",
            {R"("\u003cb\u003e \"foo%\" O'Reilly \u0026bar;")", R"("a[href =~ \"//example.com\"]#foo")", R"("Hello, \u003cb\u003eWorld\u003c/b\u003e \u0026amp;tc!")", R"(" dir=\"ltr\"")", R"(c && alert("Hello, World!");)", R"("Hello, World & O'Reilly\u0021")", "\"greeting=H%69,\\u0026addressee=(World)\"", R"("greeting=H%69,\u0026addressee=(World) 2x, https://golang.org/favicon.ico 500.5w")", R"(",foo/,")"},
        },
        {
            "<button onclick=\"alert({{.}})\">",
            {R"(&#34;\u003cb\u003e \&#34;foo%\&#34; O&#39;Reilly \u0026bar;&#34;)", R"(&#34;a[href =~ \&#34;//example.com\&#34;]#foo&#34;)", R"(&#34;Hello, \u003cb\u003eWorld\u003c/b\u003e \u0026amp;tc!&#34;)", R"(&#34; dir=\&#34;ltr\&#34;&#34;)", "c &amp;&amp; alert(&#34;Hello, World!&#34;);", R"(&#34;Hello, World &amp; O&#39;Reilly\u0021&#34;)", R"(&#34;greeting=H%69,\u0026addressee=(World)&#34;)", R"(&#34;greeting=H%69,\u0026addressee=(World) 2x, https://golang.org/favicon.ico 500.5w&#34;)", "&#34;,foo/,&#34;"},
        },
        {
            R"(<script>alert("{{.}}")</script>)",
            {R"(\u003cb\u003e \u0022foo%\u0022 O\u0027Reilly \u0026bar;)", R"(a[href =~ \u0022\/\/example.com\u0022]#foo)", R"(Hello, \u003cb\u003eWorld\u003c\/b\u003e \u0026amp;tc!)", R"( dir=\u0022ltr\u0022)", R"(c \u0026\u0026 alert(\u0022Hello, World!\u0022);)", R"(Hello, World \u0026 O\u0027Reilly\u0021)", R"(greeting=H%69,\u0026addressee=(World))", R"(greeting=H%69,\u0026addressee=(World) 2x, https:\/\/golang.org\/favicon.ico 500.5w)", R"(,foo\/,)"},
        },
        {
            R"(<script type="text/javascript">alert("{{.}}")</script>)",
            {R"(\u003cb\u003e \u0022foo%\u0022 O\u0027Reilly \u0026bar;)", R"(a[href =~ \u0022\/\/example.com\u0022]#foo)", R"(Hello, \u003cb\u003eWorld\u003c\/b\u003e \u0026amp;tc!)", R"( dir=\u0022ltr\u0022)", R"(c \u0026\u0026 alert(\u0022Hello, World!\u0022);)", R"(Hello, World \u0026 O\u0027Reilly\u0021)", R"(greeting=H%69,\u0026addressee=(World))", R"(greeting=H%69,\u0026addressee=(World) 2x, https:\/\/golang.org\/favicon.ico 500.5w)", R"(,foo\/,)"},
        },
        {
            R"(<script type="text/javascript">alert({{.}})</script>)",
            {R"("\u003cb\u003e \"foo%\" O'Reilly \u0026bar;")", R"("a[href =~ \"//example.com\"]#foo")", R"("Hello, \u003cb\u003eWorld\u003c/b\u003e \u0026amp;tc!")", R"(" dir=\"ltr\"")", R"(c && alert("Hello, World!");)", R"("Hello, World & O'Reilly\u0021")", "\"greeting=H%69,\\u0026addressee=(World)\"", R"("greeting=H%69,\u0026addressee=(World) 2x, https://golang.org/favicon.ico 500.5w")", R"(",foo/,")"},
        },
        {
            // Not treated as JS. The output is same as for <div>{{.}}</div>
            R"(<script type="text/template">{{.}}</script>)",
            {"&lt;b&gt; &#34;foo%&#34; O&#39;Reilly &amp;bar;", "a[href =~ &#34;//example.com&#34;]#foo", "Hello, <b>World</b> &amp;tc!", " dir=&#34;ltr&#34;", "c &amp;&amp; alert(&#34;Hello, World!&#34;);", R"(Hello, World &amp; O&#39;Reilly\u0021)", "greeting=H%69,&amp;addressee=(World)", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", ",foo/,"},
        },
        {
            R"(<button onclick='alert("{{.}}")'>)",
            {R"(\u003cb\u003e \u0022foo%\u0022 O\u0027Reilly \u0026bar;)", R"(a[href =~ \u0022\/\/example.com\u0022]#foo)", R"(Hello, \u003cb\u003eWorld\u003c\/b\u003e \u0026amp;tc!)", R"( dir=\u0022ltr\u0022)", R"(c \u0026\u0026 alert(\u0022Hello, World!\u0022);)", R"(Hello, World \u0026 O\u0027Reilly\u0021)", R"(greeting=H%69,\u0026addressee=(World))", R"(greeting=H%69,\u0026addressee=(World) 2x, https:\/\/golang.org\/favicon.ico 500.5w)", R"(,foo\/,)"},
        },
        {
            R"(<a href="?q={{.}}">)",
            {"%3cb%3e%20%22foo%25%22%20O%27Reilly%20%26bar%3b", "a%5bhref%20%3d~%20%22%2f%2fexample.com%22%5d%23foo", "Hello%2c%20%3cb%3eWorld%3c%2fb%3e%20%26amp%3btc%21", "%20dir%3d%22ltr%22", "c%20%26%26%20alert%28%22Hello%2c%20World%21%22%29%3b", "Hello%2c%20World%20%26%20O%27Reilly%5cu0021", "greeting=H%69,&amp;addressee=%28World%29", "greeting%3dH%2569%2c%26addressee%3d%28World%29%202x%2c%20https%3a%2f%2fgolang.org%2ffavicon.ico%20500.5w", ",foo/,"},
        },
        {
            R"(<style>body { background: url('?img={{.}}') }</style>)",
            {"%3cb%3e%20%22foo%25%22%20O%27Reilly%20%26bar%3b", "a%5bhref%20%3d~%20%22%2f%2fexample.com%22%5d%23foo", "Hello%2c%20%3cb%3eWorld%3c%2fb%3e%20%26amp%3btc%21", "%20dir%3d%22ltr%22", "c%20%26%26%20alert%28%22Hello%2c%20World%21%22%29%3b", "Hello%2c%20World%20%26%20O%27Reilly%5cu0021", "greeting=H%69,&addressee=%28World%29", "greeting%3dH%2569%2c%26addressee%3d%28World%29%202x%2c%20https%3a%2f%2fgolang.org%2ffavicon.ico%20500.5w", ",foo/,"},
        },
        {
            R"(<img srcset="{{.}}">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
        {
            "<img srcset={{.}}>",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", "&#32;dir&#61;%22ltr%22", "#ZgotmplZ,&#32;World!%22%29;", "Hello,#ZgotmplZ", "greeting&#61;H%69%2c&amp;addressee&#61;%28World%29", "greeting&#61;H%69,&amp;addressee&#61;(World)&#32;2x,&#32;https://golang.org/favicon.ico&#32;500.5w", "%2cfoo/%2c"},
        },
        {
            R"(<img srcset="{{.}} 2x, https://golang.org/ 500.5w">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
        {
            R"(<img srcset="http://godoc.org/ {{.}}, https://golang.org/ 500.5w">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
        {
            R"(<img srcset="http://godoc.org/?q={{.}} 2x, https://golang.org/ 500.5w">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
        {
            R"(<img srcset="http://godoc.org/ 2x, {{.}} 500.5w">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
        {
            R"(<img srcset="http://godoc.org/ 2x, https://golang.org/ {{.}}">)",
            {"#ZgotmplZ", "#ZgotmplZ", "Hello,#ZgotmplZ", " dir=%22ltr%22", "#ZgotmplZ, World!%22%29;", "Hello,#ZgotmplZ", "greeting=H%69%2c&amp;addressee=%28World%29", "greeting=H%69,&amp;addressee=(World) 2x, https://golang.org/favicon.ico 500.5w", "%2cfoo/%2c"},
        },
    };

    for (const auto& test : tests) {
        auto tmpl = htmpl::Must(htmpl::New("x")->Parse(test.input));
        std::size_t pre = test.input.find("{{.}}");
        std::size_t post = test.input.size() - (pre + 5);
        for (std::size_t i = 0; i < data.size(); i++) {
            strings::Builder b;
            if (auto err = tmpl->Execute(b, data[i])) {
                ADD_FAILURE() << test.input << " with " << data[i].Str() << ": " << str(err);
                continue;
            }
            std::string s = b.str().str();
            EXPECT_EQ(s.substr(pre, s.size() - post - pre), test.want[i]) << test.input << " with " << data[i].Str();
        }
    }
}

namespace {

// Test that we print using the String method. Was issue 3073.
struct myStringer {
    int v;
    string String() const { return "string=" + std::to_string(v); }
};

struct errorer {
    int v;
    string Error() const { return "error=" + std::to_string(v); }
};

}  // namespace

TEST(GoHTMLTemplate, Stringer) {
    myStringer s{3};
    auto tmpl = htmpl::Must(htmpl::New("x")->Parse("{{.}}"));
    strings::Builder b;
    ASSERT_EQ(tmpl->Execute(b, &s), nullptr);
    EXPECT_EQ(b.str().str(), "string=3");
    errorer e{7};
    strings::Builder b2;
    ASSERT_EQ(tmpl->Execute(b2, &e), nullptr);
    EXPECT_EQ(b2.str().str(), "error=7");
}

// https://golang.org/issue/5982
TEST(GoHTMLTemplate, EscapingNilNonemptyInterfaces) {
    auto tmpl = htmpl::Must(htmpl::New("x")->Parse("{{.E}}"));

    struct {
        gopp::error E;
    } testData;  // any non-empty interface here will do; error is just ready at hand
    bytes::Buffer got;
    (void)tmpl->Execute(got, testData);

    // A non-empty interface should print like an empty interface.
    struct {
        Value E;
    } data;
    bytes::Buffer want;
    (void)tmpl->Execute(want, data);

    EXPECT_EQ(asStringView(want.Bytes()), asStringView(got.Bytes()));
}

// ---------------------------------------------------------------------------
// clone_test.go

TEST(GoHTMLTemplate, AddParseTreeHTML) {
    auto root = htmpl::Must(htmpl::New("root")->Parse(R"({{define "a"}} {{.}} {{template "b"}} {{.}} "></a>{{end}})"));
    auto [tree, err] = parse::Parse("t", R"({{define "b"}}<a href="{{end}})", "", "");
    ASSERT_EQ(err, nullptr) << str(err);
    auto added = htmpl::Must(root->AddParseTree("b", tree["b"]));
    strings::Builder b;
    err = added->ExecuteTemplate(b, "a", std::string("1>0"));
    ASSERT_EQ(err, nullptr) << str(err);
    EXPECT_EQ(b.str().str(), R"( 1&gt;0 <a href=" 1%3e0 "></a>)");
}

TEST(GoHTMLTemplate, Clone) {
    // The {{.}} will be executed with data "<i>*/" in different contexts.
    // In the t0 template, it will be in a text context.
    // In the t1 template, it will be in a URL context.
    // In the t2 template, it will be in a JavaScript context.
    // In the t3 template, it will be in a CSS context.
    const std::string tmpl = R"({{define "a"}}{{template "lhs"}}{{.}}{{template "rhs"}}{{end}})";
    const std::string data = "<i>*/";
    auto run = [&](const ptr<htmpl::Template>& t) {
        strings::Builder b;
        auto err = t->ExecuteTemplate(b, "a", data);
        EXPECT_EQ(err, nullptr) << str(err);
        return b.str().str();
    };

    // Create an incomplete template t0.
    auto t0 = htmpl::Must(htmpl::New("t0")->Parse(tmpl));

    // Clone t0 as t1.
    auto t1 = htmpl::Must(t0->Clone());
    htmpl::Must(t1->Parse(R"({{define "lhs"}} <a href=" {{end}})"));
    htmpl::Must(t1->Parse(R"({{define "rhs"}} "></a> {{end}})"));

    // Execute t1.
    EXPECT_EQ(run(t1), R"( <a href=" %3ci%3e*/ "></a> )") << "t1";

    // Clone t0 as t2.
    auto t2 = htmpl::Must(t0->Clone());
    htmpl::Must(t2->Parse(R"({{define "lhs"}} <p onclick="javascript: {{end}})"));
    htmpl::Must(t2->Parse(R"({{define "rhs"}} "></p> {{end}})"));

    // Execute t2.
    EXPECT_EQ(run(t2), R"( <p onclick="javascript: &#34;\u003ci\u003e*/&#34; "></p> )") << "t2";

    // Clone t0 as t3, but do not execute t3 yet.
    auto t3 = htmpl::Must(t0->Clone());
    htmpl::Must(t3->Parse(R"({{define "lhs"}} <style> {{end}})"));
    htmpl::Must(t3->Parse(R"({{define "rhs"}} </style> {{end}})"));

    // Complete t0.
    htmpl::Must(t0->Parse(R"({{define "lhs"}} ( {{end}})"));
    htmpl::Must(t0->Parse(R"({{define "rhs"}} ) {{end}})"));

    // Clone t0 as t4. Redefining the "lhs" template should not fail.
    auto t4 = htmpl::Must(t0->Clone());
    EXPECT_EQ(t4->Parse(R"({{define "lhs"}} OK {{end}})").second, nullptr) << "redefine \"lhs\"";

    // Cloning t1 should fail as it has been executed.
    EXPECT_NE(t1->Clone().second, nullptr) << "cloning t1: got nil err want non-nil";

    // Redefining the "lhs" template in t1 should fail as it has been
    // executed.
    EXPECT_NE(t1->Parse(R"({{define "lhs"}} OK {{end}})").second, nullptr) << "redefine \"lhs\"";

    // Execute t0.
    EXPECT_EQ(run(t0), " ( &lt;i&gt;*/ ) ") << "t0";

    // Clone t0. This should fail, as t0 has already executed.
    EXPECT_NE(t0->Clone().second, nullptr) << "t0.Clone(): got nil err want non-nil";

    // Similarly, cloning sub-templates should fail.
    EXPECT_NE(t0->Lookup("a")->Clone().second, nullptr) << "t0.Lookup(\"a\").Clone()";
    EXPECT_NE(t0->Lookup("lhs")->Clone().second, nullptr) << "t0.Lookup(\"lhs\").Clone()";

    // Execute t3.
    EXPECT_EQ(run(t3), " <style> ZgotmplZ </style> ") << "t3";
}

TEST(GoHTMLTemplate, Templates) {
    std::vector<std::string> names = {"t0", "a", "lhs", "rhs"};
    // Some template definitions borrowed from TestClone.
    const std::string tmpl = R"(
		{{define "a"}}{{template "lhs"}}{{.}}{{template "rhs"}}{{end}}
		{{define "lhs"}} <a href=" {{end}}
		{{define "rhs"}} "></a> {{end}})";
    auto t0 = htmpl::Must(htmpl::New("t0")->Parse(tmpl));
    auto templates = t0->Templates();
    EXPECT_EQ(templates.size(), names.size());
    for (const auto& name : names) {
        bool found = false;
        for (const auto& t : templates) {
            if (name == t->text_->Name().view()) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "could not find template " << name;
    }
}

// This used to crash; https://golang.org/issue/3281
TEST(GoHTMLTemplate, CloneCrash) {
    auto t1 = htmpl::New("all");
    htmpl::Must(t1->New("t1")->Parse(R"({{define "foo"}}foo{{end}})"));
    (void)t1->Clone();
}

// Ensure that this guarantee from the docs is upheld: "Further calls to
// Parse in the copy will add templates to the copy but not to the
// original."
TEST(GoHTMLTemplate, CloneThenParse) {
    auto t0 = htmpl::Must(htmpl::New("t0")->Parse(R"({{define "a"}}{{template "embedded"}}{{end}})"));
    auto t1 = htmpl::Must(t0->Clone());
    htmpl::Must(t1->Parse(R"({{define "embedded"}}t1{{end}})"));
    EXPECT_EQ(t0->Templates().size() + 1, t1->Templates().size()) << "adding a template to a clone added it to the original";
    // Check that t0 has no "embedded" template.
    discard d;
    EXPECT_NE(t0->ExecuteTemplate(d, "a", nullptr), nullptr) << "expected 'no such template' error";
}

// https://golang.org/issue/5980
TEST(GoHTMLTemplate, FuncMapWorksAfterClone) {
    htmpl::FuncMap funcs = {
        {"customFunc", []() -> std::pair<string, gopp::error> { return {string(), errors::New("issue5980")}; }},
    };

    // get the expected error output (no clone)
    auto uncloned = htmpl::Must(htmpl::New("")->Funcs(funcs)->Parse("{{customFunc}}"));
    discard d;
    auto wantErr = uncloned->Execute(d, nullptr);

    // toClone must be the same as uncloned. It has to be recreated from
    // scratch, since cloning cannot occur after execution.
    auto toClone = htmpl::Must(htmpl::New("")->Funcs(funcs)->Parse("{{customFunc}}"));
    auto cloned = htmpl::Must(toClone->Clone());
    auto gotErr = cloned->Execute(d, nullptr);

    EXPECT_EQ(str(wantErr), str(gotErr)) << "clone error message mismatch";
}

// https://golang.org/issue/16101
TEST(GoHTMLTemplate, TemplateCloneExecuteRace) {
    const std::string input = R"(<title>{{block "a" .}}a{{end}}</title><body>{{block "b" .}}b{{end}}<body>)";
    const std::string overlay = R"({{define "b"}}A{{end}})";
    auto outer = htmpl::Must(htmpl::New("outer")->Parse(input));
    auto tmpl = htmpl::Must(htmpl::Must(outer->Clone())->Parse(overlay));

    sync::WaitGroup wg;
    std::atomic<int> errs{0};
    for (int i = 0; i < 10; i++) {
        wg.Add(1);
        go([&] {
            for (int j = 0; j < 100; j++) {
                discard d;
                if (tmpl->Execute(d, std::string("data"))) errs++;
            }
            wg.Done();
        });
    }
    wg.Wait();
    EXPECT_EQ(errs.load(), 0);
}

TEST(GoHTMLTemplate, TemplateCloneLookup) {
    // Template.Lookup(name) for a non-existing name should return nil.
    auto tmpl = htmpl::Must(htmpl::New("x")->Parse("a"));
    tmpl = htmpl::Must(tmpl->Clone());
    EXPECT_EQ(tmpl->Lookup(tmpl->Name()), tmpl) << "after Clone, tmpl.Lookup(tmpl.Name()) != tmpl";
}

TEST(GoHTMLTemplate, CloneGrowth) {
    auto tmpl = htmpl::Must(htmpl::New("root")->Parse(R"(<title>{{block "B". }}Arg{{end}}</title>)"));
    tmpl = htmpl::Must(tmpl->Clone());
    htmpl::Must(tmpl->Parse(R"({{define "B"}}Text{{end}})"));
    for (int i = 0; i < 10; i++) {
        discard d;
        (void)tmpl->Execute(d, nullptr);
    }
    EXPECT_LE(tmpl->DefinedTemplates().size(), 200u) << "too many templates";
}

// https://golang.org/issue/17735
TEST(GoHTMLTemplate, CloneRedefinedName) {
    const std::string base = R"(
{{ define "a" -}}<title>{{ template "b" . -}}</title>{{ end -}}
{{ define "b" }}{{ end -}}
)";
    const std::string page = R"({{ template "a" . }})";

    auto t1 = htmpl::Must(htmpl::New("a")->Parse(base));

    for (int i = 0; i < 2; i++) {
        auto t2 = htmpl::Must(t1->Clone());
        t2 = htmpl::Must(t2->New(string(std::to_string(i)))->Parse(page));
        discard d;
        auto err = t2->Execute(d, nullptr);
        ASSERT_EQ(err, nullptr) << str(err);
    }
}

// Issue 24791.
TEST(GoHTMLTemplate, ClonePipe) {
    auto a = htmpl::Must(htmpl::New("a")->Parse(R"({{define "a"}}{{range $v := .A}}{{$v}}{{end}}{{end}})"));
    struct {
        std::vector<string> A{"hi"};
    } data;
    auto b = htmpl::Must(a->Clone());
    strings::Builder buf;
    auto err = b->Execute(buf, &data);
    ASSERT_EQ(err, nullptr) << str(err);
    EXPECT_EQ(buf.str().str(), "hi");
}

// ---------------------------------------------------------------------------
// multi_test.go

namespace {

struct U {
    string V;
};

// The part of exec_test.go's tVal that the multi tests use.
struct T {
    int I = 17;
    std::vector<int> SI = {3, 4, 5};
    std::shared_ptr<U> Up = std::make_shared<U>(U{"v"});
    const U* UPtr() const { return Up.get(); }
};

struct execTest {
    std::string name;
    std::string input;
    std::string output;
    Value data;
    bool ok;
};

const T tVal;

std::vector<execTest> multiExecTests() {
    return {
        {"empty", "", "", Value(), true},
        {"text", "some text", "some text", Value(), true},
        {"invoke x", R"({{template "x" .SI}})", "TEXT", Value::Ref(tVal), true},
        {"invoke x no args", R"({{template "x"}})", "TEXT", Value::Ref(tVal), true},
        {"invoke dot int", R"({{template "dot" .I}})", "17", Value::Ref(tVal), true},
        {"invoke dot []int", R"({{template "dot" .SI}})", "[3 4 5]", Value::Ref(tVal), true},
        {"invoke dotV", R"({{template "dotV" .UPtr}})", "v", Value::Ref(tVal), true},
        {"invoke nested int", R"({{template "nested" .I}})", "17", Value::Ref(tVal), true},
        {"variable declared by template", R"({{template "nested" $x:=.SI}},{{index $x 1}})", "[3 4 5],4",
         Value::Ref(tVal), true},

        // User-defined function: test argument evaluator.
        {"testFunc literal", R"({{oneArg "joe"}})", "oneArg=joe", Value::Ref(tVal), true},
        {"testFunc .", "{{oneArg .}}", "oneArg=joe", Value::Of(std::string("joe")), true},
    };
}

void testExecute(const std::vector<execTest>& execTests, const ptr<htmpl::Template>& tmpl0) {
    htmpl::FuncMap funcs = {
        {"oneArg", [](string a) { return "oneArg=" + a; }},
    };
    for (const auto& test : execTests) {
        SCOPED_TRACE(test.name);
        ptr<htmpl::Template> tmpl;
        gopp::error err;
        if (!tmpl0) {
            std::tie(tmpl, err) = htmpl::New(test.name)->Funcs(funcs)->Parse(test.input);
        } else {
            std::tie(tmpl, err) = tmpl0->Clone();
            if (err) {
                ADD_FAILURE() << "clone error: " << str(err);
                continue;
            }
            std::tie(tmpl, err) = tmpl->New(test.name)->Funcs(funcs)->Parse(test.input);
        }
        if (err) {
            ADD_FAILURE() << "parse error: " << str(err);
            continue;
        }
        strings::Builder b;
        err = tmpl->Execute(b, test.data);
        if (!test.ok && !err) {
            ADD_FAILURE() << "expected error; got none";
            continue;
        }
        if (test.ok && err) {
            ADD_FAILURE() << "unexpected execute error: " << str(err);
            continue;
        }
        EXPECT_EQ(b.str().str(), test.output);
    }
}

const std::string multiText1 = R"(
	{{define "x"}}TEXT{{end}}
	{{define "dotV"}}{{.V}}{{end}}
)";

const std::string multiText2 = R"(
	{{define "dot"}}{{.}}{{end}}
	{{define "nested"}}{{template "dot" .}}{{end}}
)";

// Go's testdata, written to a temporary directory.
std::string testdata() {
    static std::string dir = [] {
        char tmpl_dir[] = "/tmp/gopp_html_template_testXXXXXX";
        std::string d = ::mkdtemp(tmpl_dir);
        auto write = [&](const char* name, std::string_view text) {
            std::FILE* f = std::fopen((d + "/" + name).c_str(), "w");
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        };
        write("file1.tmpl", "{{define \"x\"}}TEXT{{end}}\n{{define \"dotV\"}}{{.V}}{{end}}\n");
        write("file2.tmpl", "{{define \"dot\"}}{{.}}{{end}}\n{{define \"nested\"}}{{template \"dot\" .}}{{end}}\n");
        write("tmpl1.tmpl", "template1\n{{define \"x\"}}x{{end}}\n{{template \"y\"}}\n");
        write("tmpl2.tmpl", "template2\n{{define \"y\"}}y{{end}}\n{{template \"x\"}}\n");
        return d;
    }();
    return dir;
}

std::vector<execTest> templateFileExecTests() {
    return {
        {"test", R"({{template "tmpl1.tmpl"}}{{template "tmpl2.tmpl"}})", "template1\n\ny\ntemplate2\n\nx\n",
         Value::Of(0), true},
    };
}

}  // namespace

TEST(GoHTMLTemplate, MultiExecute) {
    // Declare a couple of templates first.
    auto [tmpl, err] = htmpl::New("root")->Parse(multiText1);
    ASSERT_EQ(err, nullptr) << "parse error for 1: " << str(err);
    err = tmpl->Parse(multiText2).second;
    ASSERT_EQ(err, nullptr) << "parse error for 2: " << str(err);
    testExecute(multiExecTests(), tmpl);
}

TEST(GoHTMLTemplate, ParseFiles) {
    EXPECT_NE(htmpl::ParseFiles({"DOES NOT EXIST"}).second, nullptr) << "expected error for non-existent file; got none";
    auto tmpl = htmpl::New("root");
    auto d = testdata();
    auto err = tmpl->ParseFiles({d + "/file1.tmpl", d + "/file2.tmpl"}).second;
    ASSERT_EQ(err, nullptr) << "error parsing files: " << str(err);
    testExecute(multiExecTests(), tmpl);
}

TEST(GoHTMLTemplate, ParseGlob) {
    EXPECT_NE(htmpl::ParseGlob("DOES NOT EXIST").second, nullptr) << "expected error for non-existent file; got none";
    EXPECT_NE(htmpl::New("error")->ParseGlob("[x").second, nullptr) << "expected error for bad pattern; got none";
    auto tmpl = htmpl::New("root");
    auto err = tmpl->ParseGlob(testdata() + "/file*.tmpl").second;
    ASSERT_EQ(err, nullptr) << "error parsing files: " << str(err);
    testExecute(multiExecTests(), tmpl);
}

TEST(GoHTMLTemplate, ParseFilesWithData) {
    auto d = testdata();
    auto [tmpl, err] = htmpl::New("root")->ParseFiles({d + "/tmpl1.tmpl", d + "/tmpl2.tmpl"});
    ASSERT_EQ(err, nullptr) << "error parsing files: " << str(err);
    testExecute(templateFileExecTests(), tmpl);
}

TEST(GoHTMLTemplate, ParseGlobWithData) {
    auto [tmpl, err] = htmpl::New("root")->ParseGlob(testdata() + "/tmpl*.tmpl");
    ASSERT_EQ(err, nullptr) << "error parsing files: " << str(err);
    testExecute(templateFileExecTests(), tmpl);
}

TEST(GoHTMLTemplate, AddParseTreeToUnparsedTemplate) {
    std::string master = "{{define \"master\"}}{{end}}";
    auto tmpl = htmpl::New("master");
    auto [tree, err] = parse::Parse("master", master, "", "");
    ASSERT_EQ(err, nullptr) << "unexpected parse err: " << str(err);
    (void)tmpl->AddParseTree("master", tree["master"]);  // used to panic
}

TEST(GoHTMLTemplate, Redefinition) {
    auto [tmpl, err] = htmpl::New("tmpl1")->Parse(R"({{define "test"}}foo{{end}})");
    ASSERT_EQ(err, nullptr) << "parse 1: " << str(err);
    err = tmpl->Parse(R"({{define "test"}}bar{{end}})").second;
    ASSERT_EQ(err, nullptr) << "got error " << str(err) << ", expected nil";
    err = tmpl->New("tmpl2")->Parse(R"({{define "test"}}bar{{end}})").second;
    ASSERT_EQ(err, nullptr) << "got error " << str(err) << ", expected nil";
}

// Issue 10879
TEST(GoHTMLTemplate, EmptyTemplateCloneCrash) {
    auto t1 = htmpl::New("base");
    (void)t1->Clone();  // used to panic
}

TEST(GoHTMLTemplate, Parse) {
    // In multiple calls to Parse with the same receiver template, only one
    // call can contain text other than space, comments, and template
    // definitions
    auto t1 = htmpl::New("test");
    ASSERT_EQ(t1->Parse(R"({{define "test"}}{{end}})").second, nullptr);
    ASSERT_EQ(t1->Parse(R"({{define "test"}}{{/* this is a comment */}}{{end}})").second, nullptr);
    ASSERT_EQ(t1->Parse(R"({{define "test"}}foo{{end}})").second, nullptr);
}

TEST(GoHTMLTemplate, EmptyTemplate) {
    struct {
        std::vector<std::string> defn;
        std::string in;
        std::string want;
    } cases[] = {
        {{"x", "y"}, "", "y"},
        {{""}, "once", ""},
        {{"", ""}, "twice", ""},
        {{"{{.}}", "{{.}}"}, "twice", "twice"},
        {{"{{/* a comment */}}", "{{/* a comment */}}"}, "comment", ""},
        {{"{{.}}", ""}, "twice", "twice"},  // TODO: should want "" not "twice"
    };

    for (std::size_t i = 0; i < std::size(cases); i++) {
        const auto& c = cases[i];
        auto root = htmpl::New("root");
        ptr<htmpl::Template> m;
        for (const auto& d : c.defn) {
            gopp::error err;
            std::tie(m, err) = root->New(string(c.in))->Parse(d);
            ASSERT_EQ(err, nullptr) << str(err);
        }
        strings::Builder buf;
        if (auto err = m->Execute(buf, c.in)) {
            ADD_FAILURE() << i << " " << str(err);
            continue;
        }
        EXPECT_EQ(buf.str().str(), c.want) << i;
    }
}

// Issue 19249 was a regression in 1.8 caused by the handling of empty
// templates added in that release, which got different answers depending
// on the order templates appeared in the internal map.
TEST(GoHTMLTemplate, Issue19294) {
    // The empty block in "xhtml" should be replaced during execution by
    // the contents of "stylesheet", but if the internal map associating
    // names with templates is built in the wrong order, the empty block
    // looks non-empty and this doesn't happen.
    std::map<std::string, std::string> inlined = {
        {"stylesheet", R"({{define "stylesheet"}}stylesheet{{end}})"},
        {"xhtml", R"({{block "stylesheet" .}}{{end}})"},
    };
    std::vector<std::string> all = {"stylesheet", "xhtml"};
    for (int i = 0; i < 100; i++) {
        auto [res, err] = htmpl::New("title.xhtml")->Parse(R"({{template "xhtml" .}})");
        ASSERT_EQ(err, nullptr) << str(err);
        for (const auto& name : all) {
            ASSERT_EQ(res->New(string(name))->Parse(inlined[name]).second, nullptr);
        }
        strings::Builder buf;
        (void)res->Execute(buf, 0);
        ASSERT_EQ(buf.str().str(), "stylesheet") << "iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// template_test.go

TEST(GoHTMLTemplate, TemplateClone) {
    auto orig = htmpl::New("name");
    auto [clone, err] = orig->Clone();
    ASSERT_EQ(err, nullptr) << str(err);
    ASSERT_EQ(clone->Templates().size(), orig->Templates().size()) << "Invalid length of t.Clone().Templates()";

    const std::string want = "stuff";
    auto parsed = htmpl::Must(clone->Parse(want));
    strings::Builder buf;
    err = parsed->Execute(buf, nullptr);
    ASSERT_EQ(err, nullptr) << str(err);
    EXPECT_EQ(buf.str().str(), want);
}

namespace {

struct testCase {
    ptr<htmpl::Template> root = htmpl::New("root");

    ptr<htmpl::Template> lookup(std::string_view name) const { return root->Lookup(name); }

    void mustParse(const ptr<htmpl::Template>& t, std::string_view text) {
        auto err = t->Parse(text).second;
        ASSERT_EQ(err, nullptr) << "parse: " << str(err);
    }

    void mustNotParse(const ptr<htmpl::Template>& t, std::string_view text) {
        ASSERT_NE(t->Parse(text).second, nullptr) << "parse: unexpected success";
    }

    void mustExecute(const ptr<htmpl::Template>& t, const Value& val, std::string_view want) {
        strings::Builder buf;
        auto err = t->Execute(buf, val);
        ASSERT_EQ(err, nullptr) << "execute: " << str(err);
        EXPECT_EQ(buf.str().view(), want) << "template output";
    }
};

}  // namespace

TEST(GoHTMLTemplate, RedefineNonEmptyAfterExecution) {
    testCase c;
    c.mustParse(c.root, "foo");
    c.mustExecute(c.root, Value(), "foo");
    c.mustNotParse(c.root, "bar");
}

TEST(GoHTMLTemplate, RedefineEmptyAfterExecution) {
    testCase c;
    c.mustParse(c.root, "");
    c.mustExecute(c.root, Value(), "");
    c.mustNotParse(c.root, "foo");
    c.mustExecute(c.root, Value(), "");
}

TEST(GoHTMLTemplate, RedefineAfterNonExecution) {
    testCase c;
    c.mustParse(c.root, R"({{if .}}<{{template "X"}}>{{end}}{{define "X"}}foo{{end}})");
    c.mustExecute(c.root, Value::Of(0), "");
    c.mustNotParse(c.root, R"({{define "X"}}bar{{end}})");
    c.mustExecute(c.root, Value::Of(1), "&lt;foo>");
}

TEST(GoHTMLTemplate, RedefineAfterNamedExecution) {
    testCase c;
    c.mustParse(c.root, R"(<{{template "X" .}}>{{define "X"}}foo{{end}})");
    c.mustExecute(c.root, Value(), "&lt;foo>");
    c.mustNotParse(c.root, R"({{define "X"}}bar{{end}})");
    c.mustExecute(c.root, Value(), "&lt;foo>");
}

TEST(GoHTMLTemplate, RedefineNestedByNameAfterExecution) {
    testCase c;
    c.mustParse(c.root, R"({{define "X"}}foo{{end}})");
    c.mustExecute(c.lookup("X"), Value(), "foo");
    c.mustNotParse(c.root, R"({{define "X"}}bar{{end}})");
    c.mustExecute(c.lookup("X"), Value(), "foo");
}

TEST(GoHTMLTemplate, RedefineNestedByTemplateAfterExecution) {
    testCase c;
    c.mustParse(c.root, R"({{define "X"}}foo{{end}})");
    c.mustExecute(c.lookup("X"), Value(), "foo");
    c.mustNotParse(c.lookup("X"), "bar");
    c.mustExecute(c.lookup("X"), Value(), "foo");
}

TEST(GoHTMLTemplate, RedefineSafety) {
    testCase c;
    c.mustParse(c.root, R"(<html><a href="{{template "X"}}">{{define "X"}}{{end}})");
    c.mustExecute(c.root, Value(), R"(<html><a href="">)");
    // Note: Every version of Go prior to Go 1.8 accepted the redefinition
    // of "X" on the next line, but luckily kept it from being used in the
    // outer template. Now we reject it, which makes clearer that we're not
    // going to use it.
    c.mustNotParse(c.root, R"({{define "X"}}" bar="baz{{end}})");
    c.mustExecute(c.root, Value(), R"(<html><a href="">)");
}

TEST(GoHTMLTemplate, RedefineTopUse) {
    testCase c;
    c.mustParse(c.root, R"({{template "X"}}{{.}}{{define "X"}}{{end}})");
    c.mustExecute(c.root, Value::Of(42), "42");
    c.mustNotParse(c.root, R"({{define "X"}}<script>{{end}})");
    c.mustExecute(c.root, Value::Of(42), "42");
}

TEST(GoHTMLTemplate, RedefineOtherParsers) {
    testCase c;
    c.mustParse(c.root, "");
    c.mustExecute(c.root, Value(), "");
    auto err = c.root->ParseFiles({"no.template"}).second;
    EXPECT_TRUE(err && str(err).find("Execute") != std::string::npos) << "ParseFiles: " << str(err);
    err = c.root->ParseGlob("*.no.template").second;
    EXPECT_TRUE(err && str(err).find("Execute") != std::string::npos) << "ParseGlob: " << str(err);
    err = c.root->AddParseTree("t1", c.root->Tree).second;
    EXPECT_TRUE(err && str(err).find("Execute") != std::string::npos) << "AddParseTree: " << str(err);
}

TEST(GoHTMLTemplate, Numbers) {
    testCase c;
    c.mustParse(c.root, "{{print 1_2.3_4}} {{print 0x0_1.e_0p+02}}");
    c.mustExecute(c.root, Value(), "12.34 7.5");
}

TEST(GoHTMLTemplate, StringsInScriptsWithJsonContentTypeAreCorrectlyEscaped) {
    // See #33671 and #37634 for more context on this.
    struct {
        std::string name, in;
    } tests[] = {
        {"empty", ""},
        {"invalid", "\uFFFD"},
        {"null", std::string(1, '\0')},
        {"unit separator", "\x1F"},
        {"tab", "\t"},
        {"gt and lt", "<>"},
        {"quotes", R"('")"},
        {"ASCII letters", "ASCII letters"},
        {"Unicode", "\u0295\u2299\u03d6\u2299\u0294"},
        {"Pizza", "\U0001F355"},
    };
    const std::string prefix = R"(<script type="application/ld+json">)";
    const std::string suffix = "</script>";
    const std::string templ = prefix + R"("{{.}}")" + suffix;
    auto tpl = htmpl::Must(htmpl::New("JS string is JSON string")->Parse(templ));
    for (const auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        bytes::Buffer buf;
        auto err = tpl->Execute(buf, tt.in);
        ASSERT_EQ(err, nullptr) << "Cannot render template: " << str(err);
        std::string_view trimmed = asStringView(buf.Bytes());
        if (trimmed.starts_with(prefix)) trimmed.remove_prefix(prefix.size());
        if (trimmed.ends_with(suffix)) trimmed.remove_suffix(suffix.size());
        std::string got;
        err = json::Unmarshal(trimmed, &got);
        ASSERT_EQ(err, nullptr) << "Cannot parse JS string " << trimmed << " as JSON: " << str(err);
        EXPECT_EQ(got, tt.in) << "Serialization changed the string value";
    }
}

TEST(GoHTMLTemplate, SkipEscapeComments) {
    testCase c;
    auto tr = parse::New("root");
    tr->Mode = parse::ParseComments;
    std::map<std::string, ptr<parse::Tree>, std::less<>> treeSet;
    auto [newT, err] = tr->Parse("{{/* A comment */}}{{ 1 }}{{/* Another comment */}}", "", "", treeSet);
    ASSERT_EQ(err, nullptr) << "Cannot parse template text: " << str(err);
    auto [root, err2] = c.root->AddParseTree("root", ptr<parse::Tree>(newT));
    ASSERT_EQ(err2, nullptr) << "Cannot add parse tree to template: " << str(err2);
    c.root = root;
    c.mustExecute(c.root, Value(), "1");
}
