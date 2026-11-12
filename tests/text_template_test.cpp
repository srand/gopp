// go++'s own text/template tests: the data model's C++ types, Go's printf
// verbs, and execution on fibers.
#include <gopp/chan.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/text/template.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

using namespace gopp;
namespace tmpl = gopp::text::tmpl;
using tmpl::Value;

namespace {

std::string run(std::string_view text, const Value& data, tmpl::FuncMap funcs = {}) {
    auto [t, err] = tmpl::New("t")->Funcs(funcs)->Parse(text);
    if (err) return "PARSE ERROR: " + err.Error().str();
    strings::Builder b;
    if (gopp::error xerr = t->Execute(b, data)) return "ERROR: " + xerr.Error().str();
    return b.str().str();
}

struct Item final : object {
    string Name;
    int Count = 0;
    std::optional<double> Price;
    std::vector<string> Tags;
    string Label() const { return Name + "#" + strconv::Itoa(Count); }
    std::pair<int, gopp::error> Div(int d) const {
        if (d == 0) return {0, errors::New("division by zero")};
        return {Count / d, nullptr};
    }
    string Join(tmpl::Variadic<string> parts) const {
        std::string out = Name.str();
        for (auto& p : parts) out += "," + p.str();
        return string(out);
    }
};

struct Inventory {
    std::vector<ptr<Item>> Items;
    std::map<string, int> Stock;
};

struct node final : object {
    int Val = 0;
    ptr<node> Next;
};

}  // namespace

TEST(TextTemplateGopp, Readme) {
    struct Inv {
        string Material;
        int Count;
    };
    EXPECT_EQ(run("{{.Count}} items are made of {{.Material}}", Value::Of(Inv{"wool", 17})), "17 items are made of wool");
}

TEST(TextTemplateGopp, StructsMethodsAndPointers) {
    auto apple = make_ptr<Item>();
    apple->Name = "apple";
    apple->Count = 10;
    apple->Price = 0.5;
    apple->Tags = {"fruit", "red"};
    auto pear = make_ptr<Item>();
    pear->Name = "pear";
    pear->Count = 3;
    Inventory inv{{apple, pear}, {{"b", 2}, {"a", 1}}};
    Value d = Value::Ref(inv);
    EXPECT_EQ(run("{{range .Items}}{{.Label}} {{end}}", d), "apple#10 pear#3 ");
    EXPECT_EQ(run("{{range .Items}}{{with .Price}}{{.}}{{else}}none{{end}};{{end}}", d), "0.5;none;");
    EXPECT_EQ(run("{{(index .Items 0).Div 3}}", d), "3");
    EXPECT_TRUE(run("{{(index .Items 0).Div 0}}", d).find("error calling Div: division by zero") != std::string::npos);
    EXPECT_EQ(run(R"({{(index .Items 1).Join "x" "y"}})", d), "pear,x,y");
    EXPECT_EQ(run("{{range $k, $v := .Stock}}{{$k}}={{$v}} {{end}}", d), "a=1 b=2 ");
    EXPECT_EQ(run("{{len .Items}} {{len (index .Items 0).Tags}}", d), "2 2");
    EXPECT_EQ(run("{{index (index .Items 0).Tags 1}}", d), "red");
}

TEST(TextTemplateGopp, RecursiveIntrusivePointers) {
    auto list = make_ptr<node>();
    list->Val = 1;
    list->Next = make_ptr<node>();
    list->Next->Val = 2;
    std::string text = R"({{define "list"}}{{.Val}}{{with .Next}},{{template "list" .}}{{end}}{{end}}{{template "list" .}})";
    EXPECT_EQ(run(text, Value::Of(list)), "1,2");
}

TEST(TextTemplateGopp, JSONValues) {
    json::Value v;
    gopp::error err = json::Unmarshal(R"({"name":"gopher","langs":["go","c++"],"age":13,"pet":null})", &v);
    ASSERT_FALSE(err) << err;
    Value d = Value::Ref(v);
    EXPECT_EQ(run("{{.name}} {{.age}} {{range .langs}}[{{.}}]{{end}} {{.pet}}", d), "gopher 13 [go][c++] <no value>");
    EXPECT_EQ(run("{{if .langs}}yes{{end}} {{len .langs}}", d), "yes 2");
}

TEST(TextTemplateGopp, Printf) {
    struct {
        std::string text, want;
    } tests[] = {
        {R"({{printf "%d|%5d|%-5d|%05d|%x|%X|%o|%b" 42 42 42 42 255 255 8 5}})", "42|   42|42   |00042|ff|FF|10|101"},
        {R"({{printf "%s|%q|%v|%10s|%-10s|" "go" "go" "go" "go" "go"}})", R"(go|"go"|go|        go|go        |)"},
        {R"({{printf "%f|%.2f|%e|%g|%v" 3.14159 3.14159 1234.5678 0.000012 1e21}})", "3.141590|3.14|1.234568e+03|1.2e-05|1e+21"},
        {R"({{printf "%t|%v" true false}})", "true|false"},
        {R"({{printf "%c|%U|%q" 65 0x1F600 'x'}})", "A|U+1F600|'x'"},
        {R"({{printf "%d"}})", "%!d(MISSING)"},
        {R"({{printf "%d" "x"}})", "%!d(string=x)"},
        {R"({{printf "%d" 1 2}})", "1%!(EXTRA int=2)"},
        {R"({{printf "%%|%v" nil}})", "%|<nil>"},
        {R"({{printf "%v %v" .S .M}})", "[1 2 3] map[a:1 b:2]"},
        {R"({{print 1 2 "a" "b" 3}})", "1 2ab3"},  // spaces only between non-strings
        {R"({{println 1 "a"}})", "1 a\n"},
    };
    struct data {
        std::vector<int> S{1, 2, 3};
        std::map<std::string, int> M{{"b", 2}, {"a", 1}};
    } dt;
    for (auto& tc : tests) EXPECT_EQ(run(tc.text, Value::Ref(dt)), tc.want) << tc.text;
}

TEST(TextTemplateGopp, FuncMapLambdas) {
    tmpl::FuncMap funcs = {
        {"upper", [](string s) { return strings::ToUpper(s); }},
        {"repeat", [](string s, int n) { return strings::Repeat(s, static_cast<std::size_t>(n)); }},
        {"sum", [](tmpl::Variadic<int> xs) {
             int s = 0;
             for (int x : xs) s += x;
             return s;
         }},
        {"fail", []() -> std::pair<string, gopp::error> { return {{}, errors::New("nope")}; }},
    };
    EXPECT_EQ(run(R"({{"go" | upper | printf "%s!"}} {{repeat "ab" 3}} {{sum 1 2 3 4}})", Value(), funcs), "GO! ababab 10");
    EXPECT_EQ(run("{{fail}}", Value(), funcs), R"(ERROR: template: t:1:2: executing "t" at <fail>: error calling fail: nope)");
    EXPECT_EQ(run(R"({{repeat "x" "y"}})", Value(), funcs),
              R"(ERROR: template: t:1:13: executing "t" at <"y">: expected integer; found "y")");
}

// Templates run on fibers, whose stacks are small: the deepest recursion
// the executor allows must fit.
TEST(TextTemplateGopp, DeepRecursionOnFiber) {
    auto done = make_chan<std::string>(1);
    go([done] {
        std::string text = R"({{define "r"}}{{if lt . 150}}.{{template "r" (add . 1)}}{{end}}{{end}}{{template "r" 0}})";
        tmpl::FuncMap funcs = {{"add", [](int a, int b) { return a + b; }}};
        std::string out = run(text, Value::Of(0), funcs);
        auto t = tmpl::Must(tmpl::New("inf")->Parse(R"({{template "inf" .}})"));
        gopp::error err = t->Execute(io::Discard(), nullptr);
        done.send(std::to_string(out.size()) + " " + (err ? err.Error().str() : std::string("nil")));
    });
    std::string got = done.recv();
    EXPECT_TRUE(got.starts_with("150 ")) << got;
    EXPECT_TRUE(got.find("exceeded maximum template depth (200)") != std::string::npos) << got;
}

TEST(TextTemplateGopp, ConcurrentExecution) {
    auto t = tmpl::Must(tmpl::New("t")->Parse("{{range .}}{{.}}{{end}}"));
    std::vector<int> data = {1, 2, 3};
    auto results = make_chan<std::string>(16);
    for (int i = 0; i < 16; i++) {
        go([t, &data, results] {
            strings::Builder b;
            t->Execute(b, data);
            results.send(b.str().str());
        });
    }
    for (int i = 0; i < 16; i++) EXPECT_EQ(results.recv(), "123");
}

// A set of associated templates refers to its templates and they to the
// set; the set and all its templates go with the last reference to any of
// them (Go's garbage collector frees such cycles).
namespace gopp::text::tmpl::detail {
long live_templates() noexcept;  // template.cpp: templates alive, for tests
}

TEST(TextTemplate, SetIsFreedWithItsLastTemplate) {
    struct access {
        static long live() { return tmpl::detail::live_templates(); }
    };
    long before = access::live();
    {
        auto t = tmpl::Must(tmpl::New("root")->Parse(R"({{define "a"}}A{{end}}{{define "b"}}B{{template "a"}}{{end}}root)"));
        auto extra = t->New("extra");
        tmpl::Must(extra->Parse("extra {{template \"b\"}}"));
        auto clone = tmpl::Must(t->Clone());
        EXPECT_EQ(t->Templates().size(), 4u);
        strings::Builder b;
        EXPECT_FALSE(clone->ExecuteTemplate(b, "extra", Value()));
        EXPECT_EQ(b.str(), "extra BA");
        EXPECT_GT(access::live(), before);
    }
    EXPECT_EQ(access::live(), before) << "templates leaked";

    // Holding one associated template keeps its whole set.
    ptr<tmpl::Template> b;
    {
        auto t = tmpl::Must(tmpl::New("root")->Parse(R"({{define "a"}}A{{end}}{{define "b"}}B{{template "a"}}{{end}})"));
        b = t->Lookup("b");
    }
    strings::Builder out;
    EXPECT_FALSE(b->Execute(out, nullptr));
    EXPECT_EQ(out.str(), "BA");
    b = nullptr;
    EXPECT_EQ(access::live(), before) << "templates leaked";
}
