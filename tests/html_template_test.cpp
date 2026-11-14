// go++'s own html/template tests: the documented example, content types
// of the data model, and execution on fibers.
#include <gopp/chan.hpp>
#include <gopp/html/template.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace gopp;
namespace htmpl = gopp::html::tmpl;

TEST(HTMLTemplate, Readme) {
    auto t = htmpl::Must(htmpl::New("page")->Parse(R"html(<a href="/search?q={{.}}" onclick="go({{.}})">{{.}}</a>)html"));
    strings::Builder b;
    ASSERT_EQ(t->Execute(b, std::string("O'Reilly & <Sons>")), nullptr);
    EXPECT_EQ(b.str().str(), R"html(<a href="/search?q=O%27Reilly%20%26%20%3cSons%3e" onclick="go(&#34;O&#39;Reilly \u0026 \u003cSons\u003e&#34;)">O&#39;Reilly &amp; &lt;Sons&gt;</a>)html");
}

namespace {

// A content type of the program's own: Markdown is plain text to
// html/template, a string of a type of its own to its functions.
struct Markdown : text::tmpl::NamedString {
    using NamedString::NamedString;
};

struct page {
    string Title;
    htmpl::HTML Body;
    htmpl::URL Link;
    std::vector<string> Tags;
    std::map<std::string, int> Counts;
    Markdown Notes;
};

}  // namespace

TEST(HTMLTemplate, ContentTypesInTheDataModel) {
    page p{"<Hi>", htmpl::HTML("<b>bold</b>"), htmpl::URL("javascript:void(0)"), {"a&b", "c"}, {{"x", 1}}, Markdown("*x*")};
    auto t = htmpl::Must(htmpl::New("p")->Parse(
        R"html(<h1>{{.Title}}</h1>{{.Body}}<a href="{{.Link}}">{{range .Tags}}[{{.}}]{{end}}</a><script>var c = {{.Counts}}, t = {{.Tags}};</script>{{.Notes}})html"));
    strings::Builder b;
    ASSERT_EQ(t->Execute(b, p), nullptr);
    EXPECT_EQ(b.str().str(),
              R"html(<h1>&lt;Hi&gt;</h1><b>bold</b><a href="javascript:void%280%29">[a&amp;b][c]</a><script>var c = {"x":1}, t = ["a\u0026b","c"];</script>*x*)html");
}

TEST(HTMLTemplate, ErrorsCarryTheirPlace) {
    auto t = htmpl::Must(htmpl::New("t")->Parse("{{if .}}<a{{end}}"));
    strings::Builder b;
    auto err = t->Execute(b, true);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err.Error().str(), "html/template:t:1:5: {{if}} branches end in different contexts: "
                                 "{stateTag delimNone urlPartNone jsCtxRegexp [] attrNone elementNone <nil>}, "
                                 "{stateText delimNone urlPartNone jsCtxRegexp [] attrNone elementNone <nil>}");
    const auto* e = errors::As<htmpl::Error>(err);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->ErrorCode, htmpl::ErrBranchEnd);
}

// Templates escape on first use, from whichever fiber gets there first.
TEST(HTMLTemplate, ConcurrentFirstExecution) {
    auto t = htmpl::Must(htmpl::New("t")->Parse(R"({{define "x"}}<i>{{.}}</i>{{end}}<p title="{{.}}">{{template "x" .}}</p>)"));
    sync::WaitGroup wg;
    std::atomic<int> bad{0};
    for (int i = 0; i < 8; i++) {
        wg.Add(1);
        go([&] {
            for (int j = 0; j < 50; j++) {
                strings::Builder b;
                if (t->Execute(b, std::string("<&>")) || b.str() != "<p title=\"&lt;&amp;&gt;\"><i>&lt;&amp;&gt;</i></p>") bad++;
            }
            wg.Done();
        });
    }
    wg.Wait();
    EXPECT_EQ(bad.load(), 0);
}
