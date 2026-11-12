// Ported from Go 1.27.3's text/template exec_test.go and multi_test.go,
// with Go's test names and tables (execTests is generated from the Go
// source). Go's test data is a struct T with fields and methods; here it
// is a C++ struct, reflected. Left out, since go++'s data model doesn't
// have them: channels and iterator functions to range over, unsafe
// pointers, maps with interface keys, non-empty interface values, typed
// nil receivers (a nil *V printing "nilV"), slice capacity, fmt.Stringer
// values from other packages, and functions returning nothing or three
// values; and %T output naming Go packages. The left-out execTests entries
// are listed in skipped_exec_tests below.
#include <gopp/errors.hpp>
#include <gopp/strings.hpp>
#include <gopp/text/template.hpp>

#include <gtest/gtest.h>

#include <array>
#include <complex>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gopp;
namespace tmpl = gopp::text::tmpl;
using tmpl::FuncMap;
using tmpl::Value;
using tmpl::Variadic;

namespace {

struct U {
    std::string V;
    std::string TrueFalse(bool b) const { return b ? "true" : ""; }
};

struct V {
    int j = 0;
    std::string String() const { return std::format("<{}>", j); }
};

struct W {
    int k = 0;
    std::string Error() const { return std::format("[{}]", k); }
};

const gopp::error myError = errors::New("my error");

struct T {
    // Basics
    bool True = false;
    int I = 0;
    uint16_t U16 = 0;
    std::string X, S;
    double FloatZero = 0;
    std::complex<double> ComplexZero;
    // Nested structs.
    const ::U* U = nullptr;
    // Struct with String method.
    ::V V0;
    const ::V* V1 = nullptr;
    const ::V* V2 = nullptr;
    // Struct with Error method.
    ::W W0;
    const ::W* W1 = nullptr;
    const ::W* W2 = nullptr;
    // Slices
    std::vector<int> SI;
    std::vector<int> SIEmpty;
    std::vector<bool> SB;
    // Arrays
    std::array<int, 3> AI{};
    const std::array<int, 3>* PAI = nullptr;
    // Maps
    std::map<std::string, int> MSI, MSIone, MSIEmpty;
    std::map<int, int> MII;
    std::map<int32_t, std::string> MI32S;
    std::map<int64_t, std::string> MI64S;
    std::map<uint32_t, std::string> MUI32S;
    std::map<uint64_t, std::string> MUI64S;
    std::map<int8_t, std::string> MI8S;
    std::map<uint8_t, std::string> MUI8S;
    std::vector<std::map<std::string, int>> SMSI;
    // Empty interfaces; used to see if we can dig inside one.
    Value Empty0;  // nil
    Value Empty1, Empty2, Empty3, Empty4;
    gopp::error Err;
    // Pointers
    const int* PI = nullptr;
    const std::string* PS = nullptr;
    const std::vector<int>* PSI = nullptr;
    const int* NIL = nullptr;
    // Function (not method)
    std::function<std::string(std::string, std::string)> BinaryFunc;
    std::function<std::string(Variadic<std::string>)> VariadicFunc;
    std::function<std::string(int, Variadic<std::string>)> VariadicFuncInt;
    std::function<bool(const int*)> NilOKFunc;
    std::function<std::pair<std::string, gopp::error>()> ErrFunc;
    std::function<std::string()> PanicFunc;

    // Simple methods with and without arguments.
    std::string Method0() const { return "M0"; }
    int Method1(int a) const { return a; }
    std::string Method2(uint16_t a, std::string b) const { return std::format("Method2: {} {}", a, b); }
    std::string Method3(Value v) const { return "Method3: " + tmpl::Sprint(std::span(&v, 1)).str(); }
    std::shared_ptr<T> Copy() const { return std::make_shared<T>(*this); }
    std::vector<int> MAdd(int a, std::vector<int> b) const {
        for (int& x : b) x += a;
        return b;
    }
    // MyError returns a value and an error according to its argument.
    std::pair<bool, gopp::error> MyError(bool error) const {
        if (error) return {true, myError};
        return {false, nullptr};
    }
    // A few methods to test chaining.
    const ::U* GetU() const { return U; }
};

const int pi_val = 23;
const std::string ps_val = "a string";
const std::vector<int> psi_val = {21, 22, 23};
const ::U u_val{"v"};
const ::V v1_val{7777};
const ::W w1_val{999};
const std::array<int, 3> pai_val = {3, 4, 5};

const T* make_tval() {
    static T t = [] {
        T t;
        t.True = true;
        t.I = 17;
        t.U16 = 16;
        t.X = "x";
        t.S = "xyz";
        t.U = &u_val;
        t.V0 = ::V{6666};
        t.V1 = &v1_val;  // leave V2 as nil
        t.W0 = ::W{888};
        t.W1 = &w1_val;  // leave W2 as nil
        t.SI = {3, 4, 5};
        t.AI = {3, 4, 5};
        t.PAI = &pai_val;
        t.SB = {true, false};
        t.MSI = {{"one", 1}, {"two", 2}, {"three", 3}};
        t.MSIone = {{"one", 1}};
        t.MII = {{1, 1}};
        t.MI32S = {{1, "one"}, {2, "two"}};
        t.MI64S = {{2, "i642"}, {3, "i643"}};
        t.MUI32S = {{2, "u322"}, {3, "u323"}};
        t.MUI64S = {{2, "ui642"}, {3, "ui643"}};
        t.MI8S = {{2, "i82"}, {3, "i83"}};
        t.MUI8S = {{2, "u82"}, {3, "u83"}};
        t.SMSI = {{{"one", 1}, {"two", 2}}, {{"eleven", 11}, {"twelve", 12}}};
        t.Empty1 = Value::Of(3);
        t.Empty2 = Value::Of(std::string("empty2"));
        t.Empty3 = Value::Of(std::vector<int>{7, 8});
        t.Empty4 = Value::Of(std::make_shared<::U>(::U{"UinEmpty"}));
        t.Err = errors::New("erroozle");
        t.PI = &pi_val;
        t.PS = &ps_val;
        t.PSI = &psi_val;
        t.BinaryFunc = [](std::string a, std::string b) { return std::format("[{}={}]", a, b); };
        t.VariadicFunc = [](Variadic<std::string> s) {
            std::string out = "<";
            for (std::size_t i = 0; i < s.size(); i++) out += (i ? "+" : "") + s[i];
            return out + ">";
        };
        t.VariadicFuncInt = [](int a, Variadic<std::string> s) {
            std::string out = std::to_string(a) + "=<";
            for (std::size_t i = 0; i < s.size(); i++) out += (i ? "+" : "") + s[i];
            return out + ">";
        };
        t.NilOKFunc = [](const int* s) { return s == nullptr; };
        t.ErrFunc = []() -> std::pair<std::string, gopp::error> { return {"bla", nullptr}; };
        t.PanicFunc = []() -> std::string { throw std::runtime_error("test panic"); };
        return t;
    }();
    return &t;
}

const T* tVal = make_tval();

// bigInt and bigUint are hex strings representing numbers either side of
// the max int boundary.
const std::string bigInt = std::format("0x{:x}", INT64_MAX);
const std::string bigUint = std::format("0x{:x}", uint64_t(INT64_MAX) + 1);

std::string zeroArgs() { return "zeroArgs"; }
std::string oneArg(std::string a) { return "oneArg=" + a; }
std::string twoArgs(std::string a, std::string b) { return "twoArgs=" + a + b; }

std::string dddArg(int a, Variadic<std::string> b) {
    // fmt.Sprintln(a, b): "1 [a b]\n"
    std::string out = std::to_string(a) + " [";
    for (std::size_t i = 0; i < b.size(); i++) out += (i ? " " : "") + b[i];
    return out + "]\n";
}

// vfunc takes a V and a *V.
std::string vfunc(::V, const ::V*) { return "vfunc"; }

// valueString takes a string, not a pointer.
std::string valueString(std::string) { return "value is ignored"; }

int returnInt() { return 7; }

int add(Variadic<int> args) {
    int sum = 0;
    for (int x : args) sum += x;
    return sum;
}

Value echo(Value arg) { return arg; }

std::map<std::string, std::string> makemap(Variadic<std::string> arg) {
    if (arg.size() % 2 != 0) throw std::runtime_error("bad makemap");
    std::map<std::string, std::string> m;
    for (std::size_t i = 0; i < arg.size(); i += 2) m[arg[i]] = arg[i + 1];
    return m;
}

Value mapOfThree() { return Value::Of(std::map<std::string, int>{{"three", 3}}); }

std::string typeOf(Value arg) { return tmpl::Sprintf("%T", std::span(&arg, 1)).str(); }

const std::string rangeTestInt = R"({{range $v := .}}{{printf "%T%d" $v $v}}{{end}})";

// What rangeTestInt prints for 5 of type I: Go's types are go++'s type
// names here (int32_t is "int").
template <typename I>
std::string rangeTestData() {
    std::string out;
    for (I i = 0; i < I(5); i++) out += std::string(Value::Of(i).TypeName()) + std::to_string(i);
    return out;
}

// Go's anonymous struct{ a int; b string }.
struct dot_struct {
    int a;
    std::string b;
};

struct exec_test {
    std::string name;
    std::string input;
    std::string output;
    Value data;
    bool ok;
};

// A Value for the test data: the C++ for Go's composite literals.
template <typename X>
Value D(X x) {
    return Value::Of(std::move(x));
}

FuncMap test_funcs() {
    return FuncMap{
        {"add", add},
        {"dddArg", dddArg},
        {"die", []() -> bool { throw std::runtime_error("die"); }},
        {"echo", echo},
        {"makemap", makemap},
        {"mapOfThree", mapOfThree},
        {"oneArg", oneArg},
        {"returnInt", returnInt},
        {"twoArgs", twoArgs},
        {"typeOf", typeOf},
        {"valueString", valueString},
        {"vfunc", vfunc},
        {"zeroArgs", zeroArgs},
    };
}

void test_execute(const std::vector<exec_test>& tests, tmpl::Template* tpl) {
    FuncMap funcs = test_funcs();
    for (auto& test : tests) {
        ptr<tmpl::Template> t = tpl ? tpl->New(string(test.name)) : tmpl::New(string(test.name));
        auto [parsed, err] = t->Funcs(funcs)->Parse(test.input);
        if (err) {
            ADD_FAILURE() << test.name << ": parse error: " << err;
            continue;
        }
        strings::Builder b;
        gopp::error xerr = parsed->Execute(b, test.data);
        if (!test.ok && !xerr) {
            ADD_FAILURE() << test.name << ": expected error; got none";
            continue;
        }
        if (test.ok && xerr) {
            ADD_FAILURE() << test.name << ": unexpected execute error: " << xerr;
            continue;
        }
        EXPECT_EQ(b.view(), test.output) << test.name << " (" << test.input << ")";
    }
}

// clang-format off
// Generated from Go 1.27.3 text/template exec_test.go.
const std::vector<exec_test> execTests = {
    {"empty", "", "", Value(), true},
    {"text", "some text", "some text", Value(), true},
    {"nil action", "{{nil}}", "", Value(), false},
    {"ideal int", "{{typeOf 3}}", "int", D(0), true},
    {"ideal float", "{{typeOf 1.0}}", "float64", D(0), true},
    {"ideal exp float", "{{typeOf 1e1}}", "float64", D(0), true},
    {"ideal complex", "{{typeOf 1i}}", "complex128", D(0), true},
    {"ideal int", "{{typeOf " + bigInt + "}}", "int", D(0), true},
    {"ideal too big", "{{typeOf " + bigUint + "}}", "", D(0), false},
    {"ideal nil without type", "{{nil}}", "", D(0), false},
    {".X", "-{{.X}}-", "-x-", D(tVal), true},
    {".U.V", "-{{.U.V}}-", "-v-", D(tVal), true},
    {"map .one", "{{.MSI.one}}", "1", D(tVal), true},
    {"map .two", "{{.MSI.two}}", "2", D(tVal), true},
    {"map .NO", "{{.MSI.NO}}", "<no value>", D(tVal), true},
    {"map .WRONG args", "{{.MSI.one 1}}", "", D(tVal), false},
    {"map .WRONG type", "{{.MII.one}}", "", D(tVal), false},
    {"dot int", "<{{.}}>", "<13>", D(13), true},
    {"dot uint", "<{{.}}>", "<14>", D(static_cast<unsigned>(14)), true},
    {"dot float", "<{{.}}>", "<15.1>", D(15.1), true},
    {"dot bool", "<{{.}}>", "<true>", D(true), true},
    {"dot complex", "<{{.}}>", "<(16.2-17i)>", D(std::complex<double>(16.2, -17)), true},
    {"dot string", "<{{.}}>", "<hello>", D("hello"), true},
    {"dot slice", "<{{.}}>", "<[-1 -2 -3]>", D(std::vector<int>{-1, -2, -3}), true},
    {"dot map", "<{{.}}>", "<map[two:22]>", D(std::map<std::string, int>{{"two", 22}}), true},
    {"dot struct", "<{{.}}>", "<{7 seven}>", D(dot_struct{7, "seven"}), true},
    {"$ int", "{{$}}", "123", D(123), true},
    {"$.I", "{{$.I}}", "17", D(tVal), true},
    {"$.U.V", "{{$.U.V}}", "v", D(tVal), true},
    {"declare in action", "{{$x := $.U.V}}{{$x}}", "v", D(tVal), true},
    {"simple assignment", "{{$x := 2}}{{$x = 3}}{{$x}}", "3", D(tVal), true},
    {"nested assignment", "{{$x := 2}}{{if true}}{{$x = 3}}{{end}}{{$x}}", "3", D(tVal), true},
    {"nested assignment changes the last declaration", "{{$x := 1}}{{if true}}{{$x := 2}}{{if true}}{{$x = 3}}{{end}}{{end}}{{$x}}", "1", D(tVal), true},
    {"V{6666}.String()", "-{{.V0}}-", "-<6666>-", D(tVal), true},
    {"&V{7777}.String()", "-{{.V1}}-", "-<7777>-", D(tVal), true},
    {"W{888}.Error()", "-{{.W0}}-", "-[888]-", D(tVal), true},
    {"&W{999}.Error()", "-{{.W1}}-", "-[999]-", D(tVal), true},
    {"*int", "{{.PI}}", "23", D(tVal), true},
    {"*string", "{{.PS}}", "a string", D(tVal), true},
    {"*[]int", "{{.PSI}}", "[21 22 23]", D(tVal), true},
    {"*[]int[1]", "{{index .PSI 1}}", "22", D(tVal), true},
    {"NIL", "{{.NIL}}", "<nil>", D(tVal), true},
    {"empty nil", "{{.Empty0}}", "<no value>", D(tVal), true},
    {"empty with int", "{{.Empty1}}", "3", D(tVal), true},
    {"empty with string", "{{.Empty2}}", "empty2", D(tVal), true},
    {"empty with slice", "{{.Empty3}}", "[7 8]", D(tVal), true},
    {"empty with struct", "{{.Empty4}}", "{UinEmpty}", D(tVal), true},
    {"empty with struct, field", "{{.Empty4.V}}", "UinEmpty", D(tVal), true},
    {"field on interface", "{{.foo}}", "<no value>", Value(), true},
    {"field on parenthesized interface", "{{(.).foo}}", "<no value>", Value(), true},
    {"unparenthesized non-function", "{{1 2}}", "", Value(), false},
    {"parenthesized non-function", "{{(1) 2}}", "", Value(), false},
    {"parenthesized non-function with no args", "{{(1)}}", "1", Value(), true},
    {".Method0", "-{{.Method0}}-", "-M0-", D(tVal), true},
    {".Method1(1234)", "-{{.Method1 1234}}-", "-1234-", D(tVal), true},
    {".Method1(.I)", "-{{.Method1 .I}}-", "-17-", D(tVal), true},
    {".Method2(3, .X)", "-{{.Method2 3 .X}}-", "-Method2: 3 x-", D(tVal), true},
    {".Method2(.U16, `str`)", "-{{.Method2 .U16 `str`}}-", "-Method2: 16 str-", D(tVal), true},
    {".Method2(.U16, $x)", "{{if $x := .X}}-{{.Method2 .U16 $x}}{{end}}-", "-Method2: 16 x-", D(tVal), true},
    {".Method3(nil constant)", "-{{.Method3 nil}}-", "-Method3: <nil>-", D(tVal), true},
    {"method on var", "{{if $x := .}}-{{$x.Method2 .U16 $x.X}}{{end}}-", "-Method2: 16 x-", D(tVal), true},
    {"method on chained var", "{{range .MSIone}}{{if $.U.TrueFalse $.True}}{{$.U.TrueFalse $.True}}{{else}}WRONG{{end}}{{end}}", "true", D(tVal), true},
    {"chained method", "{{range .MSIone}}{{if $.GetU.TrueFalse $.True}}{{$.U.TrueFalse $.True}}{{else}}WRONG{{end}}{{end}}", "true", D(tVal), true},
    {"chained method on variable", "{{with $x := .}}{{with .SI}}{{$.GetU.TrueFalse $.True}}{{end}}{{end}}", "true", D(tVal), true},
    {".NilOKFunc not nil", "{{call .NilOKFunc .PI}}", "false", D(tVal), true},
    {".NilOKFunc nil", "{{call .NilOKFunc nil}}", "true", D(tVal), true},
    {".BinaryFunc", "{{call .BinaryFunc `1` `2`}}", "[1=2]", D(tVal), true},
    {".VariadicFunc0", "{{call .VariadicFunc}}", "<>", D(tVal), true},
    {".VariadicFunc2", "{{call .VariadicFunc `he` `llo`}}", "<he+llo>", D(tVal), true},
    {".VariadicFuncInt", "{{call .VariadicFuncInt 33 `he` `llo`}}", "33=<he+llo>", D(tVal), true},
    {"if .BinaryFunc call", "{{ if .BinaryFunc}}{{call .BinaryFunc `1` `2`}}{{end}}", "[1=2]", D(tVal), true},
    {"if not .BinaryFunc call", "{{ if not .BinaryFunc}}{{call .BinaryFunc `1` `2`}}{{else}}No{{end}}", "No", D(tVal), true},
    {".ErrFunc", "{{call .ErrFunc}}", "bla", D(tVal), true},
    {"call nil", "{{call nil}}", "", D(tVal), false},
    {"empty call", "{{call}}", "", D(tVal), false},
    {"empty call after pipe valid", "{{.ErrFunc | call}}", "bla", D(tVal), true},
    {"empty call after pipe invalid", "{{1 | call}}", "", D(tVal), false},
    {".BinaryFuncTooFew", "{{call .BinaryFunc `1`}}", "", D(tVal), false},
    {".BinaryFuncTooMany", "{{call .BinaryFunc `1` `2` `3`}}", "", D(tVal), false},
    {".BinaryFuncBad0", "{{call .BinaryFunc 1 3}}", "", D(tVal), false},
    {".BinaryFuncBad1", "{{call .BinaryFunc `1` 3}}", "", D(tVal), false},
    {".VariadicFuncBad0", "{{call .VariadicFunc 3}}", "", D(tVal), false},
    {".VariadicFuncIntBad0", "{{call .VariadicFuncInt}}", "", D(tVal), false},
    {".VariadicFuncIntBad`", "{{call .VariadicFuncInt `x`}}", "", D(tVal), false},
    {".VariadicFuncNilBad", "{{call .VariadicFunc nil}}", "", D(tVal), false},
    {"pipeline", "-{{.Method0 | .Method2 .U16}}-", "-Method2: 16 M0-", D(tVal), true},
    {"pipeline func", "-{{call .VariadicFunc `llo` | call .VariadicFunc `he` }}-", "-<he+<llo>>-", D(tVal), true},
    {"nil pipeline", "{{ .Empty0 | call .NilOKFunc }}", "true", D(tVal), true},
    {"nil call arg", "{{ call .NilOKFunc .Empty0 }}", "true", D(tVal), true},
    {"bad nil pipeline", "{{ .Empty0 | .VariadicFunc }}", "", D(tVal), false},
    {"parens in pipeline", "{{printf `%d %d %d` (1) (2 | add 3) (add 4 (add 5 6))}}", "1 5 15", D(tVal), true},
    {"parens: $ in paren", "{{($).X}}", "x", D(tVal), true},
    {"parens: $.GetU in paren", "{{($.GetU).V}}", "v", D(tVal), true},
    {"parens: $ in paren in pipe", "{{($ | echo).X}}", "x", D(tVal), true},
    {"parens: spaces and args", std::string(R"__({{(makemap "up" "down" "left" "right").left}})__"), "right", D(tVal), true},
    {"if true", "{{if true}}TRUE{{end}}", "TRUE", D(tVal), true},
    {"if false", "{{if false}}TRUE{{else}}FALSE{{end}}", "FALSE", D(tVal), true},
    {"if nil", "{{if nil}}TRUE{{end}}", "", D(tVal), false},
    {"if 1", "{{if 1}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D(tVal), true},
    {"if 0", "{{if 0}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"if 1.5", "{{if 1.5}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D(tVal), true},
    {"if 0.0", "{{if .FloatZero}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"if 1.5i", "{{if 1.5i}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D(tVal), true},
    {"if 0.0i", "{{if .ComplexZero}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"if nonNilPointer", "{{if .PI}}NON-ZERO{{else}}ZERO{{end}}", "NON-ZERO", D(tVal), true},
    {"if nilPointer", "{{if .NIL}}NON-ZERO{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"if emptystring", "{{if ``}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"if string", "{{if `notempty`}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D(tVal), true},
    {"if emptyslice", "{{if .SIEmpty}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"if slice", "{{if .SI}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D(tVal), true},
    {"if emptymap", "{{if .MSIEmpty}}NON-EMPTY{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"if map", "{{if .MSI}}NON-EMPTY{{else}}EMPTY{{end}}", "NON-EMPTY", D(tVal), true},
    {"if $x with $y int", "{{if $x := true}}{{with $y := .I}}{{$x}},{{$y}}{{end}}{{end}}", "true,17", D(tVal), true},
    {"if $x with $x int", "{{if $x := true}}{{with $x := .I}}{{$x}},{{end}}{{$x}}{{end}}", "17,true", D(tVal), true},
    {"if else if", "{{if false}}FALSE{{else if true}}TRUE{{end}}", "TRUE", D(tVal), true},
    {"if else chain", "{{if eq 1 3}}1{{else if eq 2 3}}2{{else if eq 3 3}}3{{end}}", "3", D(tVal), true},
    {"print", std::string(R"__({{print "hello, print"}})__"), "hello, print", D(tVal), true},
    {"print 123", std::string(R"__({{print 1 2 3}})__"), "1 2 3", D(tVal), true},
    {"print nil", std::string(R"__({{print nil}})__"), "<nil>", D(tVal), true},
    {"println", std::string(R"__({{println 1 2 3}})__"), "1 2 3\n", D(tVal), true},
    {"printf int", std::string(R"__({{printf "%04x" 127}})__"), "007f", D(tVal), true},
    {"printf float", std::string(R"__({{printf "%g" 3.5}})__"), "3.5", D(tVal), true},
    {"printf complex", std::string(R"__({{printf "%g" 1+7i}})__"), "(1+7i)", D(tVal), true},
    {"printf string", std::string(R"__({{printf "%s" "hello"}})__"), "hello", D(tVal), true},
    {"printf function", std::string(R"__({{printf "%#q" zeroArgs}})__"), "`zeroArgs`", D(tVal), true},
    {"printf field", std::string(R"__({{printf "%s" .U.V}})__"), "v", D(tVal), true},
    {"printf method", std::string(R"__({{printf "%s" .Method0}})__"), "M0", D(tVal), true},
    {"printf dot", std::string(R"__({{with .I}}{{printf "%d" .}}{{end}})__"), "17", D(tVal), true},
    {"printf var", std::string(R"__({{with $x := .I}}{{printf "%d" $x}}{{end}})__"), "17", D(tVal), true},
    {"printf lots", std::string(R"__({{printf "%d %s %g %s" 127 "hello" 7-3i .Method0}})__"), "127 hello (7-3i) M0", D(tVal), true},
    {"html", std::string(R"__({{html "<script>alert(\"XSS\");</script>"}})__"), "&lt;script&gt;alert(&#34;XSS&#34;);&lt;/script&gt;", Value(), true},
    {"html pipeline", std::string(R"__({{printf "<script>alert(\"XSS\");</script>" | html}})__"), "&lt;script&gt;alert(&#34;XSS&#34;);&lt;/script&gt;", Value(), true},
    {"html", std::string(R"__({{html .PS}})__"), "a string", D(tVal), true},
    {"html typed nil", std::string(R"__({{html .NIL}})__"), "&lt;nil&gt;", D(tVal), true},
    {"html untyped nil", std::string(R"__({{html .Empty0}})__"), "&lt;no value&gt;", D(tVal), true},
    {"js", std::string(R"__({{js .}})__"), std::string(R"__(It\'d be nice.)__"), D(std::string(R"__(It'd be nice.)__")), true},
    {"urlquery", std::string(R"__({{"http://www.example.org/"|urlquery}})__"), "http%3A%2F%2Fwww.example.org%2F", Value(), true},
    {"not", "{{not true}} {{not false}}", "false true", Value(), true},
    {"and", "{{and false 0}} {{and 1 0}} {{and 0 true}} {{and 1 1}}", "false 0 0 1", Value(), true},
    {"or", "{{or 0 0}} {{or 1 0}} {{or 0 true}} {{or 1 1}}", "0 1 true 1", Value(), true},
    {"or short-circuit", "{{or 0 1 (die)}}", "1", Value(), true},
    {"and short-circuit", "{{and 1 0 (die)}}", "0", Value(), true},
    {"or short-circuit2", "{{or 0 0 (die)}}", "", Value(), false},
    {"and short-circuit2", "{{and 1 1 (die)}}", "", Value(), false},
    {"and pipe-true", "{{1 | and 1}}", "1", Value(), true},
    {"and pipe-false", "{{0 | and 1}}", "0", Value(), true},
    {"or pipe-true", "{{1 | or 0}}", "1", Value(), true},
    {"or pipe-false", "{{0 | or 0}}", "0", Value(), true},
    {"and undef", "{{and 1 .Unknown}}", "<no value>", Value(), true},
    {"or undef", "{{or 0 .Unknown}}", "<no value>", Value(), true},
    {"boolean if", "{{if and true 1 `hi`}}TRUE{{else}}FALSE{{end}}", "TRUE", D(tVal), true},
    {"boolean if not", "{{if and true 1 `hi` | not}}TRUE{{else}}FALSE{{end}}", "FALSE", Value(), true},
    {"boolean if pipe", "{{if true | not | and 1}}TRUE{{else}}FALSE{{end}}", "FALSE", Value(), true},
    {"slice[0]", "{{index .SI 0}}", "3", D(tVal), true},
    {"slice[1]", "{{index .SI 1}}", "4", D(tVal), true},
    {"slice[HUGE]", "{{index .SI 10}}", "", D(tVal), false},
    {"slice[WRONG]", "{{index .SI `hello`}}", "", D(tVal), false},
    {"slice[nil]", "{{index .SI nil}}", "", D(tVal), false},
    {"map[one]", "{{index .MSI `one`}}", "1", D(tVal), true},
    {"map[two]", "{{index .MSI `two`}}", "2", D(tVal), true},
    {"map[NO]", "{{index .MSI `XXX`}}", "0", D(tVal), true},
    {"map[nil]", "{{index .MSI nil}}", "", D(tVal), false},
    {"map[``]", "{{index .MSI ``}}", "0", D(tVal), true},
    {"map[WRONG]", "{{index .MSI 10}}", "", D(tVal), false},
    {"double index", "{{index .SMSI 1 `eleven`}}", "11", D(tVal), true},
    {"nil[1]", "{{index nil 1}}", "", D(tVal), false},
    {"map MI64S", "{{index .MI64S 2}}", "i642", D(tVal), true},
    {"map MI32S", "{{index .MI32S 2}}", "two", D(tVal), true},
    {"map MUI64S", "{{index .MUI64S 3}}", "ui643", D(tVal), true},
    {"map MI8S", "{{index .MI8S 3}}", "i83", D(tVal), true},
    {"map MUI8S", "{{index .MUI8S 2}}", "u82", D(tVal), true},
    {"index of an interface field", "{{index .Empty3 0}}", "7", D(tVal), true},
    {"slice[:]", "{{slice .SI}}", "[3 4 5]", D(tVal), true},
    {"slice[1:]", "{{slice .SI 1}}", "[4 5]", D(tVal), true},
    {"slice[1:2]", "{{slice .SI 1 2}}", "[4]", D(tVal), true},
    {"slice[-1:]", "{{slice .SI -1}}", "", D(tVal), false},
    {"slice[1:-2]", "{{slice .SI 1 -2}}", "", D(tVal), false},
    {"slice[1:2:-1]", "{{slice .SI 1 2 -1}}", "", D(tVal), false},
    {"slice[2:1]", "{{slice .SI 2 1}}", "", D(tVal), false},
    {"slice[2:2:1]", "{{slice .SI 2 2 1}}", "", D(tVal), false},
    {"out of range", "{{slice .SI 4 5}}", "", D(tVal), false},
    {"out of range", "{{slice .SI 2 2 5}}", "", D(tVal), false},
    {"array[:]", "{{slice .AI}}", "[3 4 5]", D(tVal), true},
    {"array[1:]", "{{slice .AI 1}}", "[4 5]", D(tVal), true},
    {"array[1:2]", "{{slice .AI 1 2}}", "[4]", D(tVal), true},
    {"pointer to array[:]", "{{slice .PAI}}", "[3 4 5]", D(tVal), true},
    {"pointer to array[1:]", "{{slice .PAI 1}}", "[4 5]", D(tVal), true},
    {"pointer to array[1:2]", "{{slice .PAI 1 2}}", "[4]", D(tVal), true},
    {"string[:]", "{{slice .S}}", "xyz", D(tVal), true},
    {"string[0:1]", "{{slice .S 0 1}}", "x", D(tVal), true},
    {"string[1:]", "{{slice .S 1}}", "yz", D(tVal), true},
    {"string[1:2]", "{{slice .S 1 2}}", "y", D(tVal), true},
    {"out of range", "{{slice .S 1 5}}", "", D(tVal), false},
    {"3-index slice of string", "{{slice .S 1 2 2}}", "", D(tVal), false},
    {"slice of an interface field", "{{slice .Empty3 0 1}}", "[7]", D(tVal), true},
    {"slice", "{{len .SI}}", "3", D(tVal), true},
    {"map", "{{len .MSI }}", "3", D(tVal), true},
    {"len of int", "{{len 3}}", "", D(tVal), false},
    {"len of nothing", "{{len .Empty0}}", "", D(tVal), false},
    {"len of an interface field", "{{len .Empty3}}", "2", D(tVal), true},
    {"with true", "{{with true}}{{.}}{{end}}", "true", D(tVal), true},
    {"with false", "{{with false}}{{.}}{{else}}FALSE{{end}}", "FALSE", D(tVal), true},
    {"with 1", "{{with 1}}{{.}}{{else}}ZERO{{end}}", "1", D(tVal), true},
    {"with 0", "{{with 0}}{{.}}{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"with 1.5", "{{with 1.5}}{{.}}{{else}}ZERO{{end}}", "1.5", D(tVal), true},
    {"with 0.0", "{{with .FloatZero}}{{.}}{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"with 1.5i", "{{with 1.5i}}{{.}}{{else}}ZERO{{end}}", "(0+1.5i)", D(tVal), true},
    {"with 0.0i", "{{with .ComplexZero}}{{.}}{{else}}ZERO{{end}}", "ZERO", D(tVal), true},
    {"with emptystring", "{{with ``}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"with string", "{{with `notempty`}}{{.}}{{else}}EMPTY{{end}}", "notempty", D(tVal), true},
    {"with emptyslice", "{{with .SIEmpty}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"with slice", "{{with .SI}}{{.}}{{else}}EMPTY{{end}}", "[3 4 5]", D(tVal), true},
    {"with emptymap", "{{with .MSIEmpty}}{{.}}{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"with map", "{{with .MSIone}}{{.}}{{else}}EMPTY{{end}}", "map[one:1]", D(tVal), true},
    {"with empty interface, struct field", "{{with .Empty4}}{{.V}}{{end}}", "UinEmpty", D(tVal), true},
    {"with $x int", "{{with $x := .I}}{{$x}}{{end}}", "17", D(tVal), true},
    {"with $x struct.U.V", "{{with $x := $}}{{$x.U.V}}{{end}}", "v", D(tVal), true},
    {"with variable and action", "{{with $x := $}}{{$y := $.U.V}}{{$y}}{{end}}", "v", D(tVal), true},
    {"with else with", "{{with 0}}{{.}}{{else with true}}{{.}}{{end}}", "true", D(tVal), true},
    {"with else with chain", "{{with 0}}{{.}}{{else with false}}{{.}}{{else with `notempty`}}{{.}}{{end}}", "notempty", D(tVal), true},
    {"range []int", "{{range .SI}}-{{.}}-{{end}}", "-3--4--5-", D(tVal), true},
    {"range empty no else", "{{range .SIEmpty}}-{{.}}-{{end}}", "", D(tVal), true},
    {"range []int else", "{{range .SI}}-{{.}}-{{else}}EMPTY{{end}}", "-3--4--5-", D(tVal), true},
    {"range empty else", "{{range .SIEmpty}}-{{.}}-{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"range []int break else", "{{range .SI}}-{{.}}-{{break}}NOTREACHED{{else}}EMPTY{{end}}", "-3-", D(tVal), true},
    {"range []int continue else", "{{range .SI}}-{{.}}-{{continue}}NOTREACHED{{else}}EMPTY{{end}}", "-3--4--5-", D(tVal), true},
    {"range []bool", "{{range .SB}}-{{.}}-{{end}}", "-true--false-", D(tVal), true},
    {"range []int method", "{{range .SI | .MAdd .I}}-{{.}}-{{end}}", "-20--21--22-", D(tVal), true},
    {"range map", "{{range .MSI}}-{{.}}-{{end}}", "-1--3--2-", D(tVal), true},
    {"range empty map no else", "{{range .MSIEmpty}}-{{.}}-{{end}}", "", D(tVal), true},
    {"range map else", "{{range .MSI}}-{{.}}-{{else}}EMPTY{{end}}", "-1--3--2-", D(tVal), true},
    {"range empty map else", "{{range .MSIEmpty}}-{{.}}-{{else}}EMPTY{{end}}", "EMPTY", D(tVal), true},
    {"range empty interface", "{{range .Empty3}}-{{.}}-{{else}}EMPTY{{end}}", "-7--8-", D(tVal), true},
    {"range empty nil", "{{range .Empty0}}-{{.}}-{{end}}", "", D(tVal), true},
    {"range $x SI", "{{range $x := .SI}}<{{$x}}>{{end}}", "<3><4><5>", D(tVal), true},
    {"range $x $y SI", "{{range $x, $y := .SI}}<{{$x}}={{$y}}>{{end}}", "<0=3><1=4><2=5>", D(tVal), true},
    {"range $x MSIone", "{{range $x := .MSIone}}<{{$x}}>{{end}}", "<1>", D(tVal), true},
    {"range $x $y MSIone", "{{range $x, $y := .MSIone}}<{{$x}}={{$y}}>{{end}}", "<one=1>", D(tVal), true},
    {"range $x PSI", "{{range $x := .PSI}}<{{$x}}>{{end}}", "<21><22><23>", D(tVal), true},
    {"declare in range", "{{range $x := .PSI}}<{{$foo:=$x}}{{$x}}>{{end}}", "<21><22><23>", D(tVal), true},
    {"range int8", rangeTestInt, rangeTestData<int8_t>(), D(static_cast<int8_t>(5)), true},
    {"range int16", rangeTestInt, rangeTestData<int16_t>(), D(static_cast<int16_t>(5)), true},
    {"range int32", rangeTestInt, rangeTestData<int32_t>(), D(static_cast<int32_t>(5)), true},
    {"range int64", rangeTestInt, rangeTestData<int64_t>(), D(static_cast<int64_t>(5)), true},
    {"range int", rangeTestInt, rangeTestData<int>(), D(static_cast<int>(5)), true},
    {"range uint8", rangeTestInt, rangeTestData<uint8_t>(), D(static_cast<uint8_t>(5)), true},
    {"range uint16", rangeTestInt, rangeTestData<uint16_t>(), D(static_cast<uint16_t>(5)), true},
    {"range uint32", rangeTestInt, rangeTestData<uint32_t>(), D(static_cast<uint32_t>(5)), true},
    {"range uint64", rangeTestInt, rangeTestData<uint64_t>(), D(static_cast<uint64_t>(5)), true},
    {"range uint", rangeTestInt, rangeTestData<unsigned>(), D(static_cast<unsigned>(5)), true},
    {"range uintptr", rangeTestInt, rangeTestData<uintptr_t>(), D(static_cast<uintptr_t>(5)), true},
    {"range uintptr(0)", std::string(R"__({{range $v := .}}{{print $v}}{{else}}empty{{end}})__"), "empty", D(static_cast<uintptr_t>(0)), true},
    {"range 5", std::string(R"__({{range $v := 5}}{{printf "%T%d" $v $v}}{{end}})__"), rangeTestData<int>(), Value(), true},
    {"or as if true", std::string(R"__({{or .SI "slice is empty"}})__"), "[3 4 5]", D(tVal), true},
    {"or as if false", std::string(R"__({{or .SIEmpty "slice is empty"}})__"), "slice is empty", D(tVal), true},
    {"error method, error", "{{.MyError true}}", "", D(tVal), false},
    {"error method, no error", "{{.MyError false}}", "false", D(tVal), true},
    {"decimal", "{{print 1234}}", "1234", D(tVal), true},
    {"decimal _", "{{print 12_34}}", "1234", D(tVal), true},
    {"binary", "{{print 0b101}}", "5", D(tVal), true},
    {"binary _", "{{print 0b_1_0_1}}", "5", D(tVal), true},
    {"BINARY", "{{print 0B101}}", "5", D(tVal), true},
    {"octal0", "{{print 0377}}", "255", D(tVal), true},
    {"octal", "{{print 0o377}}", "255", D(tVal), true},
    {"octal _", "{{print 0o_3_7_7}}", "255", D(tVal), true},
    {"OCTAL", "{{print 0O377}}", "255", D(tVal), true},
    {"hex", "{{print 0x123}}", "291", D(tVal), true},
    {"hex _", "{{print 0x1_23}}", "291", D(tVal), true},
    {"HEX", "{{print 0X123ABC}}", "1194684", D(tVal), true},
    {"float", "{{print 123.4}}", "123.4", D(tVal), true},
    {"float _", "{{print 0_0_1_2_3.4}}", "123.4", D(tVal), true},
    {"hex float", "{{print +0x1.ep+2}}", "7.5", D(tVal), true},
    {"hex float _", "{{print +0x_1.e_0p+0_2}}", "7.5", D(tVal), true},
    {"HEX float", "{{print +0X1.EP+2}}", "7.5", D(tVal), true},
    {"print multi", "{{print 1_2_3_4 7.5_00_00_00}}", "1234 7.5", D(tVal), true},
    {"print multi2", "{{print 1234 0x0_1.e_0p+02}}", "1234 7.5", D(tVal), true},
    {"bug0", "{{range .MSIone}}{{if $.Method1 .}}X{{end}}{{end}}", "X", D(tVal), true},
    {"bug3", "{{with $}}{{.Method0}}{{end}}", "M0", D(tVal), true},
    {"bug4", "{{if .Empty0}}non-nil{{else}}nil{{end}}", "nil", D(tVal), true},
    {"bug5a", "{{.Err}}", "erroozle", D(tVal), true},
    {"bug6a", "{{vfunc .V0 .V1}}", "vfunc", D(tVal), true},
    {"bug6b", "{{vfunc .V0 .V0}}", "vfunc", D(tVal), true},
    {"bug6c", "{{vfunc .V1 .V0}}", "vfunc", D(tVal), true},
    {"bug6d", "{{vfunc .V1 .V1}}", "vfunc", D(tVal), true},
    {"bug7a", "{{3 2}}", "", D(tVal), false},
    {"bug7b", "{{$x := 1}}{{$x 2}}", "", D(tVal), false},
    {"bug7c", "{{$x := 1}}{{3 | $x}}", "", D(tVal), false},
    {"bug8a", "{{3|oneArg}}", "", D(tVal), false},
    {"bug8b", "{{4|dddArg 3}}", "", D(tVal), false},
    {"bug9", "{{.cause}}", "neglect", D(std::map<std::string, std::string>{{"cause", "neglect"}}), true},
    {"bug10", "{{mapOfThree.three}}-{{(mapOfThree).three}}", "3-3", D(0), true},
    {"bug11", "{{valueString .PS}}", "", D(T{}), false},
    {"bug12xe", "{{printf `%T` 0xef}}", "int", D(T{}), true},
    {"bug12xE", "{{printf `%T` 0xEE}}", "int", D(T{}), true},
    {"bug12Xe", "{{printf `%T` 0Xef}}", "int", D(T{}), true},
    {"bug12XE", "{{printf `%T` 0XEE}}", "int", D(T{}), true},
    {"bug13", "{{print (.Copy).I}}", "17", D(tVal), true},
    {"bug14a", "{{(nil).True}}", "", D(tVal), false},
    {"bug14b", "{{$x := nil}}{{$x.anything}}", "", D(tVal), false},
    {"bug14c", std::string(R"__({{$x := (1.0)}}{{$y := ("hello")}}{{$x.anything}}{{$y.true}})__"), "", D(tVal), false},
    {"bug15", "{{valueString returnInt}}", "", D(tVal), false},
    {"bug16a", "{{true|printf}}", "", D(tVal), false},
    {"bug16b", "{{1|printf}}", "", D(tVal), false},
    {"bug16c", "{{1.1|printf}}", "", D(tVal), false},
    {"bug16d", "{{'x'|printf}}", "", D(tVal), false},
    {"bug16e", "{{0i|printf}}", "", D(tVal), false},
    {"bug16f", "{{true|twoArgs \"xxx\"}}", "", D(tVal), false},
    {"bug16g", "{{\"aaa\" |twoArgs \"bbb\"}}", "twoArgs=bbbaaa", D(tVal), true},
    {"bug16h", "{{1|oneArg}}", "", D(tVal), false},
    {"bug16i", "{{\"aaa\"|oneArg}}", "oneArg=aaa", D(tVal), true},
    {"bug16j", "{{1+2i|printf \"%v\"}}", "(1+2i)", D(tVal), true},
    {"bug16k", "{{\"aaa\"|printf }}", "aaa", D(tVal), true},
    {"bug18a", "{{eq . '.'}}", "true", D(static_cast<int32_t>('.')), true},
    {"bug18b", "{{eq . 'e'}}", "true", D(static_cast<int32_t>('e')), true},
    {"bug18c", "{{eq . 'P'}}", "true", D(static_cast<int32_t>('P')), true},
    {"issue56490", "{{$i := 0}}{{$x := 0}}{{range $i = .AI}}{{end}}{{$i}}", "5", D(tVal), true},
    {"issue60801", "{{$k := 0}}{{$v := 0}}{{range $k, $v = .AI}}{{$k}}={{$v}} {{end}}", "0=3 1=4 2=5 ", D(tVal), true},
};

// Go execTests that use features go++ leaves out (see the top).
[[maybe_unused]] const char* const skipped_exec_tests[] = {
    ".unexported",
    "map .one interface",
    "(*V)(nil).String()",
    "(*W)(nil).Error()",
    ".Method3(nil value)",
    "method on nil value from slice",
    "method on typed nil interface value",
    "Interface Call",
    "if on typed nil interface value",
    "if UPI",
    "if EmptyUPI",
    "if map unset",
    "if map not unset",
    "len(s) < indexes < cap(s)",
    "len(s) < indexes < cap(s)",
    "indexes > cap(s)",
    "indexes > cap(s)",
    "with on typed nil interface value",
    "range count",
    "range nil count",
    "range iter.Seq[int]",
    "i = range iter.Seq[int]",
    "range iter.Seq[int] over two var",
    "i, c := range iter.Seq2[int,int]",
    "i, c = range iter.Seq2[int,int]",
    "i = range iter.Seq2[int,int]",
    "i := range iter.Seq2[int,int]",
    "i,c,x range iter.Seq2[int,int]",
    "i,x range iter.Seq[int]",
    "range iter.Seq[int] else",
    "range iter.Seq2[int,int] else",
    "bug1",
    "bug2",
    "bug5",
    "bug17a",
    "bug17b",
    "bug17c",
    "bug17d",
    "bug17e",
};

struct cmp_struct {
    unsigned Uthree = 3, Ufour = 4;
    int NegOne = -1, Three = 3;
    std::shared_ptr<int> Ptr = std::make_shared<int>(0);
    const int* NilPtr = nullptr;
    std::map<int, int> NonNilMap;
    std::map<int, int>* Map = nullptr;
    ::V V1, V2;
};

struct cmp_test {
    std::string expr;
    std::string truth;
    bool ok;
};

const std::vector<cmp_test> cmpTests = {
    {"eq true true", "true", true},
    {"eq true false", "false", true},
    {"eq 1+2i 1+2i", "true", true},
    {"eq 1+2i 1+3i", "false", true},
    {"eq 1.5 1.5", "true", true},
    {"eq 1.5 2.5", "false", true},
    {"eq 1 1", "true", true},
    {"eq 1 2", "false", true},
    {"eq `xy` `xy`", "true", true},
    {"eq `xy` `xyz`", "false", true},
    {"eq .Uthree .Uthree", "true", true},
    {"eq .Uthree .Ufour", "false", true},
    {"eq 3 4 5 6 3", "true", true},
    {"eq 3 4 5 6 7", "false", true},
    {"ne true true", "false", true},
    {"ne true false", "true", true},
    {"ne 1+2i 1+2i", "false", true},
    {"ne 1+2i 1+3i", "true", true},
    {"ne 1.5 1.5", "false", true},
    {"ne 1.5 2.5", "true", true},
    {"ne 1 1", "false", true},
    {"ne 1 2", "true", true},
    {"ne `xy` `xy`", "false", true},
    {"ne `xy` `xyz`", "true", true},
    {"ne .Uthree .Uthree", "false", true},
    {"ne .Uthree .Ufour", "true", true},
    {"lt 1.5 1.5", "false", true},
    {"lt 1.5 2.5", "true", true},
    {"lt 1 1", "false", true},
    {"lt 1 2", "true", true},
    {"lt `xy` `xy`", "false", true},
    {"lt `xy` `xyz`", "true", true},
    {"lt .Uthree .Uthree", "false", true},
    {"eq 2 2.0", "", false},                  // Different types.
    {"lt true true", "", false},              // Unordered types.
    {"lt 1+0i 1+0i", "", false},              // Unordered types.
    {"eq .Ptr 1", "", false},                 // Incompatible types.
    {"eq .Ptr .NegOne", "", false},           // Incompatible types.
    {"eq .Map .V1", "", false},               // Uncomparable types.
    {"eq .NonNilMap .NonNilMap", "", false},  // Uncomparable types.
};

// A binary tree, to walk with a recursive template.
struct Tree {
    int Val = 0;
    const Tree* Left = nullptr;
    const Tree* Right = nullptr;
};

// Use different delimiters to test Set.Delims. Also test the trimming of
// leading and trailing spaces.
const std::string treeTemplate = R"(
	(- define "tree" -)
	[
		(- .Val -)
		(- with .Left -)
			(template "tree" . -)
		(- end -)
		(- with .Right -)
			(- template "tree" . -)
		(- end -)
	]
	(- end -)
)";

const std::string execErrorText = R"(line 1
line 2
line 3
{{template "one" .}}
{{define "one"}}{{template "two" .}}{{end}}
{{define "two"}}{{template "three" .}}{{end}}
{{define "three"}}{{index "hi" $}}{{end}})";

const gopp::error alwaysError = errors::New("always be failing");

struct error_writer final : io::Writer {
    std::pair<isize, gopp::error> Write(std::span<const byte>) override { return {0, alwaysError}; }
};

struct custom_error final : error_interface {
    string Error() const override { return string::from_static("heyo !"); }
};

int funcNameTestFunc() { return 0; }

// multi_test.go

struct multi_parse_test {
    std::string name;
    std::string input;
    bool ok;
    std::vector<std::string> names;
    std::vector<std::string> results;
};

const std::vector<exec_test>& multiExecTests() {
    static const std::vector<exec_test> tests = {
        {"empty", "", "", Value(), true},
        {"text", "some text", "some text", Value(), true},
        {"invoke x", R"({{template "x" .SI}})", "TEXT", D(tVal), true},
        {"invoke x no args", R"({{template "x"}})", "TEXT", D(tVal), true},
        {"invoke dot int", R"({{template "dot" .I}})", "17", D(tVal), true},
        {"invoke dot []int", R"({{template "dot" .SI}})", "[3 4 5]", D(tVal), true},
        {"invoke dotV", R"({{template "dotV" .U}})", "v", D(tVal), true},
        {"invoke nested int", R"({{template "nested" .I}})", "17", D(tVal), true},
        {"variable declared by template", R"({{template "nested" $x:=.SI}},{{index $x 1}})", "[3 4 5],4", D(tVal), true},
        // User-defined function: test argument evaluator.
        {"testFunc literal", R"({{oneArg "joe"}})", "oneArg=joe", D(tVal), true},
        {"testFunc .", R"({{oneArg .}})", "oneArg=joe", D(std::string("joe")), true},
    };
    return tests;
}

// These strings are also in Go's testdata/*.
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
        char tmpl_dir[] = "/tmp/gopp_template_testXXXXXX";
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

const std::vector<exec_test> templateFileExecTests = {
    {"test", R"({{template "tmpl1.tmpl"}}{{template "tmpl2.tmpl"}})", "template1\n\ny\ntemplate2\n\nx\n", D(0), true},
};

const std::string cloneText1 = R"({{define "a"}}{{template "b"}}{{template "c"}}{{end}})";
const std::string cloneText2 = R"({{define "b"}}b{{end}})";
const std::string cloneText3 = R"({{define "c"}}root{{end}})";
const std::string cloneText4 = R"({{define "c"}}clone{{end}})";
const std::string cloneText5 = R"({{define "e"}}{{.Foo}}{{end}})";

}  // namespace

TEST(TextTemplate, Execute) { test_execute(execTests, nullptr); }

TEST(TextTemplate, Delims) {
    const std::string hello = "Hello, world";
    struct value_t {
        std::string Str;
    } value{hello};
    const char* delim_pairs[] = {"", "", "{{", "}}", "<<", ">>", "|", "|", "(日)", "(本)"};
    for (std::size_t i = 0; i < std::size(delim_pairs); i += 2) {
        std::string left = delim_pairs[i], right = delim_pairs[i + 1];
        std::string true_left = left.empty() ? "{{" : left, true_right = right.empty() ? "}}" : right;
        std::string action = true_left + ".Str" + true_right;
        // A comment, which is not preserved in the parse tree.
        std::string comment = true_left + "/*comment*/" + true_right;
        // An action containing a string that looks like the left delimiter.
        std::string str_action = true_left + "\"" + true_left + "\"" + true_right;
        std::string text = action + comment + str_action;
        auto [t, err] = tmpl::New("delims")->Delims(left, right)->Parse(text);
        ASSERT_FALSE(err) << "delim " << left << " text " << text << " parse err " << err;
        // The parse tree's String form roundtrips, modulo the comment.
        EXPECT_EQ(t->Tree->Root->String(), action + str_action);
        strings::Builder b;
        ASSERT_FALSE(t->Execute(b, value));
        EXPECT_EQ(b.view(), hello + true_left);
    }
}

// Check that an error from a method flows back to the top.
TEST(TextTemplate, ExecuteError) {
    strings::Builder b;
    auto t = tmpl::New("error");
    auto [p, err] = t->Parse("{{.MyError true}}");
    ASSERT_FALSE(err) << err;
    gopp::error xerr = t->Execute(b, D(tVal));
    ASSERT_TRUE(xerr);
    EXPECT_TRUE(strings::Contains(xerr.Error(), myError.Error())) << xerr;
}

// Check that an error from a nested template contains all the relevant
// information.
TEST(TextTemplate, ExecError) {
    auto [t, err] = tmpl::New("top")->Parse(execErrorText);
    ASSERT_FALSE(err) << err;
    strings::Builder b;
    gopp::error xerr = t->Execute(b, D(5));  // 5 is out of range indexing "hi"
    ASSERT_TRUE(xerr);
    EXPECT_EQ(xerr.Error(), R"(template: top:7:20: executing "three" at <index "hi" $>: error calling index: index out of range: 5)");
}

// Check that a custom error can be returned.
TEST(TextTemplate, ExecError_CustomError) {
    auto t = tmpl::Must(tmpl::New("top")
                            ->Funcs({{"err", []() -> std::pair<std::string, gopp::error> {
                                         return {"", make_ptr<custom_error>()};
                                     }}})
                            ->Parse("{{ err }}"));
    strings::Builder b;
    gopp::error err = t->Execute(b, nullptr);
    EXPECT_TRUE(errors::As<custom_error>(err)) << "expected custom error; got " << err;
}

TEST(TextTemplate, JSEscaping) {
    struct {
        std::string in, exp;
    } tests[] = {
        {"a", "a"},
        {"'foo", R"(\'foo)"},
        {R"(Go "jump" \)", R"(Go \"jump\" \\)"},
        {R"(Yukihiro says "今日は世界")", R"(Yukihiro says \"今日は世界\")"},
        {"unprintable \xEF\xBF\xBE", R"(unprintable \uFFFE)"},
        {"<html>", R"(\u003Chtml\u003E)"},
        {"no = in attributes", R"(no \u003D in attributes)"},
        {"&#x27; does not become HTML entity", R"(\u0026#x27; does not become HTML entity)"},
    };
    for (auto& tc : tests) EXPECT_EQ(tmpl::JSEscapeString(tc.in), tc.exp) << tc.in;
}

TEST(TextTemplate, Tree) {
    const Tree t4{4}, t3{3, &t4}, t6{6}, t5{5, &t6}, t2{2, &t3, &t5};
    const Tree t9{9}, t8{8, &t9}, t11{11}, t10{10, &t11}, t7{7, &t8, &t10};
    const Tree tree{1, &t2, &t7};
    auto [t, err] = tmpl::New("root")->Delims("(", ")")->Parse(treeTemplate);
    ASSERT_FALSE(err) << err;
    const std::string expect = "[1[2[3[4]][5[6]]][7[8[9]][10[11]]]]";
    // First by looking up the template.
    strings::Builder b;
    ASSERT_FALSE(t->Lookup("tree")->Execute(b, &tree));
    EXPECT_EQ(b.view(), expect);
    // Then direct to execution.
    b.reset();
    ASSERT_FALSE(t->ExecuteTemplate(b, "tree", &tree));
    EXPECT_EQ(b.view(), expect);
}

TEST(TextTemplate, ExecuteOnNewTemplate) {
    // This is issue 3872.
    tmpl::New("Name")->Templates();
    // This is issue 11379.
    make_ptr<tmpl::Template>(string())->Templates();
    make_ptr<tmpl::Template>(string())->Parse("");
    make_ptr<tmpl::Template>(string())->New(string::from_static("abc"))->Parse("");
    EXPECT_TRUE(make_ptr<tmpl::Template>(string())->Execute(io::Discard(), nullptr));  // an error, not a crash
    EXPECT_TRUE(make_ptr<tmpl::Template>(string())->ExecuteTemplate(io::Discard(), "XXX", Value()));
}

TEST(TextTemplate, MessageForExecuteEmpty) {
    // Test a truly empty template.
    auto t = tmpl::New("empty");
    strings::Builder b;
    gopp::error err = t->Execute(b, D(0));
    ASSERT_TRUE(err) << "expected initial error";
    EXPECT_EQ(err.Error(), R"(template: empty: "empty" is an incomplete or empty template)");
    // Add a non-empty template to check that the error is helpful.
    auto [tests, perr] = tmpl::New("")->Parse(R"({{define "one"}}one{{end}}{{define "two"}}two{{end}})");
    ASSERT_FALSE(perr) << perr;
    t->AddParseTree("secondary", tests->Tree);
    err = t->Execute(b, D(0));
    ASSERT_TRUE(err) << "expected second error";
    EXPECT_EQ(err.Error(), R"(template: empty: "empty" is an incomplete or empty template)");
    // Make sure we can execute the secondary.
    EXPECT_FALSE(t->ExecuteTemplate(b, "secondary", D(0)));
}

TEST(TextTemplate, FinalForPrintf) {
    auto [t, err] = tmpl::New("")->Parse(R"({{"x" | printf}})");
    ASSERT_FALSE(err) << err;
    strings::Builder b;
    EXPECT_FALSE(t->Execute(b, D(0)));
}

TEST(TextTemplate, Comparison) {
    cmp_struct data;
    for (auto& test : cmpTests) {
        std::string text = "{{if " + test.expr + "}}true{{else}}false{{end}}";
        auto [t, err] = tmpl::New("empty")->Parse(text);
        ASSERT_FALSE(err) << test.expr << ": " << err;
        strings::Builder b;
        gopp::error xerr = t->Execute(b, &data);
        if (test.ok && xerr) {
            ADD_FAILURE() << test.expr << " errored incorrectly: " << xerr;
            continue;
        }
        if (!test.ok && !xerr) {
            ADD_FAILURE() << test.expr << " did not error";
            continue;
        }
        EXPECT_EQ(b.view(), test.truth) << test.expr;
    }
}

TEST(TextTemplate, MissingMapKey) {
    std::map<std::string, int> data = {{"x", 99}};
    auto [t, err] = tmpl::New("t1")->Parse("{{.x}} {{.y}}");
    ASSERT_FALSE(err) << err;
    strings::Builder b;
    // By default, just get "<no value>".
    ASSERT_FALSE(t->Execute(b, data));
    EXPECT_EQ(b.view(), "99 <no value>");
    // Same if we set the option explicitly to the default.
    t->Option("missingkey=default");
    b.reset();
    ASSERT_FALSE(t->Execute(b, data));
    EXPECT_EQ(b.view(), "99 <no value>");
    // Next we ask for a zero value.
    t->Option("missingkey=zero");
    b.reset();
    ASSERT_FALSE(t->Execute(b, data));
    EXPECT_EQ(b.view(), "99 0");
    // Now we ask for an error.
    t->Option("missingkey=error");
    EXPECT_TRUE(t->Execute(b, data));
    // Same option, but now a nil interface: ask for an error.
    EXPECT_TRUE(t->Execute(b, nullptr));
}

// The error message for a multiline unterminated string refers to the line
// number of the opening quote.
TEST(TextTemplate, UnterminatedStringError) {
    auto [t, err] = tmpl::New("X")->Parse("hello\n\n{{`unterminated\n\n\n\n}}\n some more\n\n");
    ASSERT_TRUE(err);
    EXPECT_TRUE(strings::Contains(err.Error(), "X:3: unterminated raw quoted string")) << err;
}

TEST(TextTemplate, ExecuteGivesExecError) {
    // First, a non-execution error shouldn't be an ExecError.
    auto [t, err] = tmpl::New("X")->Parse("hello");
    ASSERT_FALSE(err) << err;
    error_writer ew;
    gopp::error xerr = t->Execute(ew, D(0));
    ASSERT_TRUE(xerr);
    EXPECT_EQ(xerr.Error(), "always be failing");
    EXPECT_FALSE(errors::As<tmpl::ExecError>(xerr));
    // This one should be an ExecError.
    auto [t2, err2] = tmpl::New("X")->Parse("hello, {{.X.Y}}");
    ASSERT_FALSE(err2) << err2;
    xerr = t2->Execute(io::Discard(), D(0));
    ASSERT_TRUE(xerr);
    EXPECT_TRUE(errors::As<tmpl::ExecError>(xerr)) << xerr;
    EXPECT_TRUE(strings::Contains(xerr.Error(), "field X in type int")) << xerr;
}

TEST(TextTemplate, GoodFuncNames) {
    for (std::string name : {"_", "a", "a1", "a1", "Ӵ"}) {
        EXPECT_NE(tmpl::New("X")->Funcs({{name, funcNameTestFunc}}), nullptr) << name;
    }
}

TEST(TextTemplate, BadFuncNames) {
    // Funcs panics for a bad name, as Go's does; a gopp panic ends the
    // process, so death tests check it.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    for (std::string name : {"", "2", "a-b"}) {
        EXPECT_DEATH(tmpl::New("X")->Funcs({{name, funcNameTestFunc}}), "is not a valid identifier") << name;
    }
}

TEST(TextTemplate, IsTrue) {
    const int* nil_ptr = nullptr;
    struct {
        Value v;
        bool want;
    } tests[] = {
        {D(1), true},
        {D(0), false},
        {D(uint8_t(1)), true},
        {D(uint8_t(0)), false},
        {D(1.0), true},
        {D(0.0), false},
        {D(std::complex<double>(1.0)), true},
        {D(std::complex<double>(0.0)), false},
        {D(true), true},
        {D(false), false},
        {D(std::array<int, 2>{1, 2}), true},
        {D(std::array<int, 0>{}), false},
        {D(std::vector<uint8_t>{'a', 'b', 'c'}), true},
        {D(std::vector<uint8_t>{}), false},
        {D(std::map<std::string, int>{{"a", 1}, {"b", 2}}), true},
        {D(std::map<std::string, int>{}), false},
        {D(std::make_shared<int>(0)), true},
        {D(nil_ptr), false},
    };
    for (auto& tc : tests) EXPECT_EQ(tmpl::IsTrue(tc.v).first, tc.want) << tc.v.TypeName();
}

TEST(TextTemplate, Block) {
    const std::string input = R"(a({{block "inner" .}}bar({{.}})baz{{end}})b)";
    const std::string want = "a(bar(hello)baz)b";
    const std::string overlay = R"({{define "inner"}}foo({{.}})bar{{end}})";
    const std::string want2 = "a(foo(goodbye)bar)b";
    auto [t, err] = tmpl::New("outer")->Parse(input);
    ASSERT_FALSE(err) << err;
    auto [t2, err2] = tmpl::Must(t->Clone())->Parse(overlay);
    ASSERT_FALSE(err2) << err2;
    strings::Builder buf;
    ASSERT_FALSE(t->Execute(buf, std::string("hello")));
    EXPECT_EQ(buf.view(), want);
    buf.reset();
    ASSERT_FALSE(t2->Execute(buf, std::string("goodbye")));
    EXPECT_EQ(buf.view(), want2);
}

TEST(TextTemplate, EvalFieldErrors) {
    // go++'s type names have no package: *T for Go's *template.T.
    const T* nil_t = nullptr;
    const std::map<std::string, std::string>* nil_map = nullptr;
    std::map<std::string, std::string> empty_map;
    struct {
        std::string name, src;
        Value value;
        std::string want;
    } tests[] = {
        // An invalid field on a nil pointer is a field error, not a
        // distracting nil pointer error (https://golang.org/issue/15125).
        {"MissingFieldOnNil", "{{.MissingField}}", D(nil_t), "can't evaluate field MissingField in type *T"},
        {"MissingFieldOnNonNil", "{{.MissingField}}", D(std::make_shared<T>()), "can't evaluate field MissingField in type *T"},
        {"ExistingFieldOnNil", "{{.X}}", D(nil_t), "nil pointer evaluating *T.X"},
        {"MissingKeyOnNilMap", "{{.MissingKey}}", D(nil_map), "nil pointer evaluating *map[string]string.MissingKey"},
        {"MissingKeyOnNilMapPtr", "{{.MissingKey}}", D(nil_map), "nil pointer evaluating *map[string]string.MissingKey"},
        {"MissingKeyOnMapPtrToNil", "{{.MissingKey}}", D(&empty_map), "<nil>"},
    };
    for (auto& tc : tests) {
        auto t = tmpl::Must(tmpl::New("tmpl")->Parse(tc.src));
        gopp::error err = t->Execute(io::Discard(), tc.value);
        std::string got = err ? err.Error().str() : "<nil>";
        EXPECT_TRUE(got.ends_with(tc.want)) << tc.name << ": got error " << got << ", want " << tc.want;
    }
}

TEST(TextTemplate, MaxExecDepth) {
    auto t = tmpl::Must(tmpl::New("tmpl")->Parse(R"({{template "tmpl" .}})"));
    gopp::error err = t->Execute(io::Discard(), nullptr);
    ASSERT_TRUE(err);
    EXPECT_TRUE(strings::Contains(err.Error(), "exceeded maximum template depth")) << err;
}

TEST(TextTemplate, AddrOfIndex) {
    // golang.org/issue/14916: .String is found on V values, also those
    // index returns.
    for (std::string text : {"{{range .}}{{.String}}{{end}}", "{{with index . 0}}{{.String}}{{end}}"}) {
        auto t = tmpl::Must(tmpl::New("tmpl")->Parse(text));
        strings::Builder buf;
        ASSERT_FALSE(t->Execute(buf, Value::Of(std::vector<::V>{::V{1}}))) << text;
        EXPECT_EQ(buf.view(), "<1>") << text;
    }
}

TEST(TextTemplate, InterfaceValues) {
    // golang.org/issue/17714.
    struct {
        std::string text, out;
    } tests[] = {
        {"{{index .Nil 1}}", "ERROR: index of untyped nil"},
        {"{{index .Slice 2}}", "2"},
        {"{{index .Slice .Two}}", "2"},
        {"{{call .Nil 1}}", "ERROR: call of nil"},
        {"{{call .PlusOne 1}}", "2"},
        {"{{call .PlusOne .One}}", "2"},
        {"{{and (index .Slice 0) true}}", "0"},
        {"{{and .Zero true}}", "0"},
        {"{{and (index .Slice 1) false}}", "false"},
        {"{{and .One false}}", "false"},
        {"{{or (index .Slice 0) false}}", "false"},
        {"{{or .Zero false}}", "false"},
        {"{{or (index .Slice 1) true}}", "1"},
        {"{{or .One true}}", "1"},
        {"{{not (index .Slice 0)}}", "true"},
        {"{{not .Zero}}", "true"},
        {"{{not (index .Slice 1)}}", "false"},
        {"{{not .One}}", "false"},
        {"{{eq (index .Slice 0) .Zero}}", "true"},
        {"{{eq (index .Slice 1) .One}}", "true"},
        {"{{ne (index .Slice 0) .Zero}}", "false"},
        {"{{ne (index .Slice 1) .One}}", "false"},
        {"{{ge (index .Slice 0) .One}}", "false"},
        {"{{ge (index .Slice 1) .Zero}}", "true"},
        {"{{gt (index .Slice 0) .One}}", "false"},
        {"{{gt (index .Slice 1) .Zero}}", "true"},
        {"{{le (index .Slice 0) .One}}", "true"},
        {"{{le (index .Slice 1) .Zero}}", "false"},
        {"{{lt (index .Slice 0) .One}}", "true"},
        {"{{lt (index .Slice 1) .Zero}}", "false"},
    };
    std::map<std::string, Value> data = {
        {"PlusOne", D(std::function<int(int)>([](int n) { return n + 1; }))},
        {"Slice", D(std::vector<int>{0, 1, 2, 3})},
        {"One", D(1)},
        {"Two", D(2)},
        {"Nil", Value()},
        {"Zero", D(0)},
    };
    for (auto& tt : tests) {
        auto t = tmpl::Must(tmpl::New("tmpl")->Parse(tt.text));
        strings::Builder buf;
        gopp::error err = t->Execute(buf, data);
        if (tt.out.starts_with("ERROR:")) {
            std::string e = tt.out.substr(7);
            EXPECT_TRUE(err && strings::Contains(err.Error(), e)) << tt.text << ": Execute: " << err << ", want error " << e;
            continue;
        }
        ASSERT_FALSE(err) << tt.text << ": " << err;
        EXPECT_EQ(buf.view(), tt.out) << tt.text;
    }
}

// Exceptions thrown during calls are returned as errors, as Go recovers
// panics. Left out: methods on nil receivers, and nil interfaces.
TEST(TextTemplate, ExecutePanicDuringCall) {
    FuncMap funcs = {{"doPanic", []() -> std::string { throw std::runtime_error("custom panic string"); }}};
    const T* nil_t = nullptr;
    struct {
        std::string name, input;
        Value data;
        std::string want_err;
    } tests[] = {
        {"direct func call panics", "{{doPanic}}", D(nil_t),
         R"(template: t:1:2: executing "t" at <doPanic>: error calling doPanic: custom panic string)"},
        {"indirect func call panics", "{{call doPanic}}", D(nil_t),
         R"(template: t:1:7: executing "t" at <doPanic>: error calling doPanic: custom panic string)"},
        {"func field call panics", "{{call .PanicFunc}}", D(tVal),
         R"(template: t:1:2: executing "t" at <call .PanicFunc>: error calling call: test panic)"},
    };
    for (auto& tc : tests) {
        auto [t, err] = tmpl::New("t")->Funcs(funcs)->Parse(tc.input);
        ASSERT_FALSE(err) << err;
        strings::Builder b;
        gopp::error xerr = t->Execute(b, tc.data);
        ASSERT_TRUE(xerr) << tc.name << ": expected error; got none";
        EXPECT_TRUE(strings::Contains(xerr.Error(), tc.want_err)) << tc.name << ": got " << xerr;
    }
}

// Left out: functions returning nothing, three values, or a non-error
// second value, which go++'s Func can't hold.
TEST(TextTemplate, FunctionCheckDuringCall) {
    struct {
        std::string name, input;
        Value data;
        std::string want_err;
    } tests[] = {
        {"call nothing", "{{call}}", D(tVal), "wrong number of args for call: want at least 1 got 0"},
        {"call non-function", "{{call .True}}", D(tVal), "error calling call: non-function .True of type bool"},
        {"call func with wrong argument", "{{call .BinaryFunc 1}}", D(tVal),
         "error calling call: wrong number of args for .BinaryFunc: got 1 want 2"},
        {"call variadic func with wrong argument", "{{call .VariadicFuncInt}}", D(tVal),
         "error calling call: wrong number of args for .VariadicFuncInt: got 0 want at least 1"},
        {"call pipeline", R"({{call (len "test")}})", Value(), R"(error calling call: non-function len "test" of type int)"},
    };
    for (auto& tc : tests) {
        auto [t, err] = tmpl::New("t")->Parse(tc.input);
        ASSERT_FALSE(err) << err;
        strings::Builder b;
        gopp::error xerr = t->Execute(b, tc.data);
        ASSERT_TRUE(xerr) << tc.name << ": expected error; got none";
        EXPECT_TRUE(strings::Contains(xerr.Error(), tc.want_err)) << tc.name << ": got " << xerr;
    }
}

// Issue 31810: a parenthesized first argument behaves properly.
TEST(TextTemplate, Issue31810) {
    // A simple value with no arguments is fine.
    strings::Builder b;
    auto [t, err] = tmpl::New("")->Parse("{{ (.)  }}");
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(t->Execute(b, std::string("result")));
    EXPECT_EQ(b.view(), "result");
    // Even a plain function fails - need to use call.
    std::function<std::string()> f = [] { return std::string("result"); };
    b.reset();
    EXPECT_TRUE(t->Execute(b, f)) << "expected error with no call, got none";
    // Works if the function is explicitly called.
    auto [t2, err2] = tmpl::New("")->Parse("{{ (call .)  }}");
    ASSERT_FALSE(err2) << err2;
    b.reset();
    ASSERT_FALSE(t2->Execute(b, f));
    EXPECT_EQ(b.view(), "result");
}

// Issue 39807: data race in html/template & text/template.
TEST(TextTemplate, Issue39807) {
    auto [foo, err] = tmpl::New("foo")->Parse(R"({{ template "bar" . }})");
    ASSERT_FALSE(err) << err;
    auto [bar, err2] = tmpl::New("bar")->Parse("bar");
    ASSERT_FALSE(err2) << err2;
    sync::WaitGroup wg;
    std::atomic<int> failures{0};
    for (int i = 1; i <= 10; i++) {
        wg.Go([&] {
            for (int j = 0; j < 10; j++) {
                if (foo->AddParseTree(bar->Name(), bar->Tree).second) failures++;
                if (foo->Execute(io::Discard(), nullptr)) failures++;
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(failures.load(), 0);
}

// multi_test.go

TEST(TextTemplate, MultiParse) {
    std::vector<multi_parse_test> tests = {
        {"empty", "", true, {}, {}},
        {"one", R"({{define "foo"}} FOO {{end}})", true, {"foo"}, {" FOO "}},
        {"two", R"({{define "foo"}} FOO {{end}}{{define "bar"}} BAR {{end}})", true, {"foo", "bar"}, {" FOO ", " BAR "}},
        // errors
        {"missing end", R"({{define "foo"}} FOO )", false, {}, {}},
        {"malformed name", R"({{define "foo}} FOO )", false, {}, {}},
    };
    for (auto& test : tests) {
        auto [t, err] = tmpl::New("root")->Parse(test.input);
        if (!err && !test.ok) {
            ADD_FAILURE() << test.name << ": expected error; got none";
            continue;
        }
        if (err && test.ok) {
            ADD_FAILURE() << test.name << ": unexpected error: " << err;
            continue;
        }
        if (err) continue;
        ASSERT_EQ(t->Templates().size(), test.names.size() + 1) << test.name;  // +1 for root
        for (std::size_t i = 0; i < test.names.size(); i++) {
            auto nt = t->Lookup(test.names[i]);
            ASSERT_TRUE(nt) << test.name << ": can't find template " << test.names[i];
            EXPECT_EQ(nt->Tree->Root->String(), test.results[i]) << test.name;
        }
    }
}

TEST(TextTemplate, MultiExecute) {
    // Declare a couple of templates first.
    auto [t, err] = tmpl::New("root")->Parse(multiText1);
    ASSERT_FALSE(err) << err;
    auto [t2, err2] = t->Parse(multiText2);
    ASSERT_FALSE(err2) << err2;
    test_execute(multiExecTests(), t.get());
}

TEST(TextTemplate, ParseFiles) {
    EXPECT_TRUE(tmpl::ParseFiles({"DOES NOT EXIST"}).second) << "expected error for non-existent file; got none";
    auto t = tmpl::New("root");
    std::string d = testdata();
    auto [p, err] = t->ParseFiles({d + "/file1.tmpl", d + "/file2.tmpl"});
    ASSERT_FALSE(err) << err;
    test_execute(multiExecTests(), t.get());
}

TEST(TextTemplate, ParseGlob) {
    EXPECT_TRUE(tmpl::ParseGlob("DOES NOT EXIST").second) << "expected error for non-existent file; got none";
    EXPECT_TRUE(tmpl::New("error")->ParseGlob("[x").second) << "expected error for bad pattern; got none";
    auto t = tmpl::New("root");
    auto [p, err] = t->ParseGlob(testdata() + "/file*.tmpl");
    ASSERT_FALSE(err) << err;
    test_execute(multiExecTests(), t.get());
}

TEST(TextTemplate, ParseFilesWithData) {
    std::string d = testdata();
    auto [t, err] = tmpl::New("root")->ParseFiles({d + "/tmpl1.tmpl", d + "/tmpl2.tmpl"});
    ASSERT_FALSE(err) << err;
    test_execute(templateFileExecTests, t.get());
}

TEST(TextTemplate, ParseGlobWithData) {
    auto [t, err] = tmpl::New("root")->ParseGlob(testdata() + "/tmpl*.tmpl");
    ASSERT_FALSE(err) << err;
    test_execute(templateFileExecTests, t.get());
}

TEST(TextTemplate, Clone) {
    // Create some templates and clone the root.
    auto [root, err] = tmpl::New("root")->Parse(cloneText1);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(root->Parse(cloneText2).second);
    root->Parse(cloneText5);
    root->Option("missingkey=error");
    auto clone = tmpl::Must(root->Clone());
    // Add variants to both.
    ASSERT_FALSE(root->Parse(cloneText3).second);
    ASSERT_FALSE(clone->Parse(cloneText4).second);
    // Verify that the clone is self-consistent.
    for (auto& v : clone->Templates()) {
        EXPECT_EQ(v->Lookup(v->Name()), v) << "clone does not contain self for " << v->Name();
    }
    EXPECT_EQ(clone->Lookup(clone->Name()), clone) << "clone does not contain root";
    // Execute root.
    strings::Builder b;
    ASSERT_FALSE(root->ExecuteTemplate(b, "a", D(0)));
    EXPECT_EQ(b.view(), "broot");
    // Execute copy.
    b.reset();
    ASSERT_FALSE(clone->ExecuteTemplate(b, "a", D(0)));
    EXPECT_EQ(b.view(), "bclone");
    b.reset();
    std::map<std::string, Value> empty;
    gopp::error root_err = root->ExecuteTemplate(b, "e", empty);
    gopp::error clone_err = clone->ExecuteTemplate(b, "e", empty);
    ASSERT_TRUE(clone_err) << "expected error from missing key in cloned template";
    ASSERT_TRUE(root_err);
    EXPECT_EQ(clone_err.Error(), root_err.Error());
}

TEST(TextTemplate, AddParseTree) {
    // Create some templates.
    auto [root, err] = tmpl::New("root")->Parse(cloneText1);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(root->Parse(cloneText2).second);
    // Add a new parse tree.
    tmpl::parse::FuncNames builtins = {"printf"};
    auto [tree, perr] = tmpl::parse::Parse("cloneText3", cloneText3, "", "", {&builtins});
    ASSERT_FALSE(perr) << perr;
    auto [added, aerr] = root->AddParseTree("c", tree["c"]);
    ASSERT_FALSE(aerr) << aerr;
    // Execute.
    strings::Builder b;
    ASSERT_FALSE(added->ExecuteTemplate(b, "a", D(0)));
    EXPECT_EQ(b.view(), "broot");
}

// Issue 7032
TEST(TextTemplate, AddParseTreeToUnparsedTemplate) {
    std::string master = R"({{define "master"}}{{end}})";
    auto t = tmpl::New("master");
    auto [tree, err] = tmpl::parse::Parse("master", master, "", "");
    ASSERT_FALSE(err) << err;
    t->AddParseTree("master", tree["master"]);  // used to panic
}

TEST(TextTemplate, Redefinition) {
    auto [t, err] = tmpl::New("tmpl1")->Parse(R"({{define "test"}}foo{{end}})");
    ASSERT_FALSE(err) << "parse 1: " << err;
    EXPECT_FALSE(t->Parse(R"({{define "test"}}bar{{end}})").second);
    EXPECT_FALSE(t->New(string::from_static("tmpl2"))->Parse(R"({{define "test"}}bar{{end}})").second);
}

// Issue 10879
TEST(TextTemplate, EmptyTemplateCloneCrash) {
    auto t1 = tmpl::New("base");
    t1->Clone();  // used to panic
}

// Issue 10910, 10926
TEST(TextTemplate, TemplateLookUp) {
    auto t1 = tmpl::New("foo");
    EXPECT_FALSE(t1->Lookup("foo")) << "Lookup returned non-nil value for undefined template foo";
    t1->New(string::from_static("bar"));
    EXPECT_FALSE(t1->Lookup("bar")) << "Lookup returned non-nil value for undefined template bar";
    t1->Parse(R"({{define "foo"}}test{{end}})");
    EXPECT_TRUE(t1->Lookup("foo")) << "Lookup returned nil value for defined template";
}

TEST(TextTemplate, New) {
    // A template with the same name already exists.
    auto [t1, err] = tmpl::New("test")->Parse(R"({{define "test"}}foo{{end}})");
    ASSERT_FALSE(err) << err;
    auto t2 = t1->New(string::from_static("test"));
    EXPECT_TRUE(t1->Tree) << "defined template got nil Tree";
    EXPECT_FALSE(t2->Tree) << "undefined template got non-nil Tree";
    bool contains_t1 = false;
    for (auto& t : t1->Templates()) {
        EXPECT_NE(t, t2) << "Templates included undefined template";
        if (t == t1) contains_t1 = true;
    }
    EXPECT_TRUE(contains_t1) << "Templates didn't include defined template";
}

TEST(TextTemplate, Parse) {
    // In multiple calls to Parse with the same receiver template, only one
    // call can contain text other than space, comments, and template
    // definitions.
    auto t1 = tmpl::New("test");
    EXPECT_FALSE(t1->Parse(R"({{define "test"}}{{end}})").second);
    EXPECT_FALSE(t1->Parse(R"({{define "test"}}{{/* this is a comment */}}{{end}})").second);
    EXPECT_FALSE(t1->Parse(R"({{define "test"}}foo{{end}})").second);
}

TEST(TextTemplate, EmptyTemplate) {
    struct {
        std::vector<std::string> defn;
        std::string in, want;
    } cases[] = {
        {{"x", "y"}, "", "y"},
        {{""}, "once", ""},
        {{"", ""}, "twice", ""},
        {{"{{.}}", "{{.}}"}, "twice", "twice"},
        {{"{{/* a comment */}}", "{{/* a comment */}}"}, "comment", ""},
        {{"{{.}}", ""}, "twice", ""},
    };
    for (auto& c : cases) {
        auto root = tmpl::New("root");
        ptr<tmpl::Template> m;
        for (auto& d : c.defn) {
            auto [r, err] = root->New(string(c.in))->Parse(d);
            ASSERT_FALSE(err) << err;
            m = r;
        }
        strings::Builder buf;
        ASSERT_FALSE(m->Execute(buf, c.in));
        EXPECT_EQ(buf.view(), c.want);
    }
}

// Issue 19249: the empty block in "xhtml" is replaced by "stylesheet"
// whatever the order of the internal map.
TEST(TextTemplate, Issue19294) {
    std::map<std::string, std::string> inlined = {
        {"stylesheet", R"({{define "stylesheet"}}stylesheet{{end}})"},
        {"xhtml", R"({{block "stylesheet" .}}{{end}})"},
    };
    for (int i = 0; i < 100; i++) {
        auto [res, err] = tmpl::New("title.xhtml")->Parse(R"({{template "xhtml" .}})");
        ASSERT_FALSE(err) << err;
        for (std::string name : {"stylesheet", "xhtml"}) ASSERT_FALSE(res->New(string(name))->Parse(inlined[name]).second);
        strings::Builder buf;
        res->Execute(buf, D(0));
        ASSERT_EQ(buf.view(), "stylesheet") << "iteration " << i;
    }
}

// Issue 48436
TEST(TextTemplate, AddToZeroTemplate) {
    tmpl::parse::FuncNames builtins = {"printf"};
    auto [tree, err] = tmpl::parse::Parse("c", cloneText3, "", "", {&builtins});
    ASSERT_FALSE(err) << err;
    auto t = make_ptr<tmpl::Template>(string());
    t->AddParseTree("x", tree["c"]);
}
