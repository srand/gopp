// Tests ported from Go 1.27.3: encoding/json/scanner_test.Go,
// encode_test.Go, stream_test.Go and decode_test.Go. Tests of features
// that go++ doesn't have (embedded structs, Number, TextMarshaler, Token,
// omitzero, interface values) aren't ported.
//
// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in Go's LICENSE file.
//
// JSON \u escapes are written with a doubled backslash in ordinary string
// literals, and non-ASCII characters as \x escapes.
#include <gopp/encoding/json.hpp>
#include <gopp/io.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <regex>
#include <vector>

using namespace gopp;

namespace {

std::string marshal_str(const auto& v) {
    auto [b, err] = json::Marshal(v);
    EXPECT_FALSE(err) << err;
    return std::string(as_string_view(b));
}

std::string nlines(std::string_view s, int n) {
    if (n <= 0) return "";
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\n' && --n == 0) return std::string(s.substr(0, i + 1));
    }
    return std::string(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// scanner_test.Go

TEST(GoEncodingJson, Valid) {
    struct {
        std::string data;
        bool ok;
    } tests[] = {
        {"foo", false},
        {"}{", false},
        {"{]", false},
        {"{}", true},
        {R"({"foo":"bar"})", true},
        {R"({"foo":"bar","bar":{"baz":["qux"]}})", true},
    };
    for (auto& tt : tests) EXPECT_EQ(json::Valid(tt.data), tt.ok) << tt.data;
}

TEST(GoEncodingJson, CompactAndIndent) {
    struct {
        std::string compact, indent;
    } tests[] = {
        {"1", "1"},
        {"{}", "{}"},
        {"[]", "[]"},
        {R"({"":2})", "{\n\t\"\": 2\n}"},
        {"[3]", "[\n\t3\n]"},
        {"[1,2,3]", "[\n\t1,\n\t2,\n\t3\n]"},
        {R"({"x":1})", "{\n\t\"x\": 1\n}"},
        {R"([true,false,null,"x",1,1.5,0,-5e+2])", "[\n\ttrue,\n\tfalse,\n\tnull,\n\t\"x\",\n\t1,\n\t1.5,\n\t0,\n\t-5e+2\n]"},
        // See golang.org/issue/34070
        {"{\"\":\"<>&\xe2\x80\xa8\xe2\x80\xa9\"}", "{\n\t\"\": \"<>&\xe2\x80\xa8\xe2\x80\xa9\"\n}"},
        // See golang.org/issue/13520 and golang.org/issue/74806
        {"null", "null \n\r\t"},
    };
    for (auto& tt : tests) {
        std::string buf;
        auto err = json::Compact(buf, tt.compact);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(buf, tt.compact) << "Compact";

        buf.clear();
        err = json::Compact(buf, tt.indent);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(buf, tt.compact) << "Compact";

        buf.clear();
        err = json::Indent(buf, tt.indent, "", "\t");
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(buf, tt.indent) << "Indent";

        buf.clear();
        err = json::Indent(buf, tt.compact, "", "\t");
        EXPECT_FALSE(err) << err;
        std::string want = tt.indent;
        while (!want.empty() && std::string_view(" \n\r\t").find(want.back()) != std::string_view::npos) want.pop_back();
        EXPECT_EQ(buf, want) << "Indent";
    }
}

TEST(GoEncodingJson, CompactSeparators) {
    // U+2028 and U+2029 should be escaped inside strings.
    // They should not appear outside strings.
    struct {
        std::string in, compact;
    } tests[] = {
        {"{\"\xe2\x80\xa8\": 1}", "{\"\xe2\x80\xa8\":1}"},
        {"{\"\xe2\x80\xa9\" :2}", "{\"\xe2\x80\xa9\":2}"},
    };
    for (auto& tt : tests) {
        std::string buf;
        EXPECT_FALSE(json::Compact(buf, tt.in));
        EXPECT_EQ(buf, tt.compact);
    }
}

TEST(GoEncodingJson, IndentErrors) {
    struct {
        std::string in, msg;
        int64 offset;
    } tests[] = {
        {R"({"X": "foo", "Y"})", "invalid character '}' after object key", int64(std::string_view(R"({"X": "foo", "Y"})").size())},
        {R"({"X": "foo" "Y": "bar"})", "invalid character '\"' after object key:value pair",
         int64(std::string_view(R"({"X": "foo" ")").size())},
    };
    for (auto& tt : tests) {
        std::string slice = "<before>";
        auto err = json::Indent(slice, tt.in, "", "  ");
        ASSERT_TRUE(err);
        auto* se = errors::As<json::SyntaxError>(err);
        ASSERT_TRUE(se) << err;
        EXPECT_EQ(se->msg, tt.msg);
        EXPECT_EQ(se->offset, tt.offset);
        EXPECT_EQ(slice, "<before>");  // unchanged on error
    }
}

// ---------------------------------------------------------------------------
// encode_test.Go

namespace {

struct nothing {};

struct OptionalsEmpty {
    [[=json::tag("sr")]] string Sr;
    [[=json::tag("so,omitempty")]] string So;
    [[=json::tag("-")]] string Sw;

    [[=json::tag("omitempty")]] int Ir;  // actually named omitempty, not an option
    [[=json::tag("io,omitempty")]] int Io;

    [[=json::tag("slr,random")]] slice<string> Slr;
    [[=json::tag("slo,omitempty")]] slice<string> Slo;

    [[=json::tag("mr")]] std::map<string, json::Value> Mr;
    [[=json::tag(",omitempty")]] std::map<string, json::Value> Mo;

    [[=json::tag("fr")]] double Fr;
    [[=json::tag("fo,omitempty")]] double Fo;

    [[=json::tag("br")]] bool Br;
    [[=json::tag("bo,omitempty")]] bool Bo;

    [[=json::tag("ur")]] unsigned Ur;
    [[=json::tag("uo,omitempty")]] unsigned Uo;

    [[=json::tag("str")]] nothing Str;
    [[=json::tag("sto,omitempty")]] nothing Sto;
};

}  // namespace

TEST(GoEncodingJson, OmitEmpty) {
    const char* want = R"({
 "sr": "",
 "omitempty": 0,
 "slr": null,
 "mr": {},
 "fr": 0,
 "br": false,
 "ur": 0,
 "str": {},
 "sto": {}
})";
    OptionalsEmpty o{};
    o.Sw = "something";
    auto [got, err] = json::MarshalIndent(o, "", " ");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(as_string_view(got), want);
}

namespace {

// Go's StringTag, without NumberStr: go++ has no json.Number.
struct StringTag {
    [[=json::tag(",string")]] bool BoolStr;
    [[=json::tag(",string")]] int64 IntStr;
    [[=json::tag(",string")]] uintptr UintptrStr;
    [[=json::tag(",string")]] string StrStr;
    friend bool operator==(const StringTag&, const StringTag&) = default;
};

}  // namespace

TEST(GoEncodingJson, RoundtripStringTag) {
    struct {
        std::string name;
        StringTag in;
        std::string want;
    } tests[] = {
        {"AllTypes", {true, 42, 44, "xzbit"}, R"({
	"BoolStr": "true",
	"IntStr": "42",
	"UintptrStr": "44",
	"StrStr": "\"xzbit\""
})"},
        // See golang.org/issues/38173.
        {"StringDoubleEscapes", {false, 0, 0, "\b\f\n\r\t\"\\"}, R"({
	"BoolStr": "false",
	"IntStr": "0",
	"UintptrStr": "0",
	"StrStr": "\"\\b\\f\\n\\r\\t\\\"\\\\\""
})"},
    };
    for (auto& tt : tests) {
        auto [got, err] = json::MarshalIndent(tt.in, "", "\t");
        ASSERT_FALSE(err) << tt.name << ": " << err;
        EXPECT_EQ(as_string_view(got), tt.want) << tt.name;
        // Verify that it round-trips.
        StringTag s2{};
        ASSERT_FALSE(json::Unmarshal(got, &s2)) << tt.name;
        EXPECT_TRUE(s2 == tt.in) << tt.name;
    }
}

namespace {

struct cycle_node final : object {
    ptr<cycle_node> next;
};

}  // namespace

TEST(GoEncodingJson, UnsupportedValues) {
    for (double v : {std::nan(""), -HUGE_VAL, HUGE_VAL}) {
        auto [b, err] = json::Marshal(v);
        ASSERT_TRUE(err) << v;
        EXPECT_TRUE(errors::As<json::UnsupportedValueError>(err)) << err;
    }
    // pointerCycle
    auto n = make_ptr<cycle_node>();
    n->next = n;
    auto [b, err] = json::Marshal(n);
    n->next = nullptr;  // break the cycle to free it
    ASSERT_TRUE(err);
    EXPECT_TRUE(errors::As<json::UnsupportedValueError>(err)) << err;
    EXPECT_NE(std::string(err.Error().view()).find("encountered a cycle"), std::string::npos) << err;
}

TEST(GoEncodingJson, HTMLEscape) {
    std::string b;
    std::string m = "{\"M\":\"<html>foo &\xe2\x80\xa8 \xe2\x80\xa9</html>\"}";
    std::string want = "{\"M\":\"\\u003chtml\\u003efoo \\u0026\\u2028 \\u2029\\u003c/html\\u003e\"}";
    json::HTMLEscape(b, m);
    EXPECT_EQ(b, want);
}

TEST(GoEncodingJson, EncodePointerString) {
    // golang.org/issue/8582
    struct stringPointer {
        [[=json::tag("n,string")]] std::shared_ptr<int64> N;
    };
    auto [b, err] = json::Marshal(stringPointer{std::make_shared<int64>(42)});
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(as_string_view(b), R"({"n":"42"})");
    stringPointer back;
    ASSERT_FALSE(json::Unmarshal(b, &back));
    ASSERT_TRUE(back.N);
    EXPECT_EQ(*back.N, 42);
}

TEST(GoEncodingJson, EncodeString) {
    // encodeStringTests: "\x00" to "\x1f".
    const char* want[32] = {
        "\"\\u0000\"", "\"\\u0001\"", "\"\\u0002\"", "\"\\u0003\"", "\"\\u0004\"", "\"\\u0005\"", "\"\\u0006\"",
        "\"\\u0007\"", "\"\\b\"",     "\"\\t\"",     "\"\\n\"",     "\"\\u000b\"", "\"\\f\"",     "\"\\r\"",
        "\"\\u000e\"", "\"\\u000f\"", "\"\\u0010\"", "\"\\u0011\"", "\"\\u0012\"", "\"\\u0013\"", "\"\\u0014\"",
        "\"\\u0015\"", "\"\\u0016\"", "\"\\u0017\"", "\"\\u0018\"", "\"\\u0019\"", "\"\\u001a\"", "\"\\u001b\"",
        "\"\\u001c\"", "\"\\u001d\"", "\"\\u001e\"", "\"\\u001f\"",
    };
    for (int c = 0; c < 32; c++) EXPECT_EQ(marshal_str(std::string(1, static_cast<char>(c))), want[c]) << c;
}

TEST(GoEncodingJson, MarshalFloat) {
    // Issue 13783
    const char* bad_res[] = {
        "p",                       // no binary exponential notation
        "^\\+",                    // no leading + sign
        "^-?0[^.]",                // no unnecessary leading zeros
        "^-?\\.",                  // leading zero required before decimal point
        "\\.(e|$)",                // no trailing decimal
        "\\.[0-9]+0(e|$)",         // no trailing zero in fraction
        "^-?(0|[0-9]{2,})\\..*e",  // exponential notation must have normalized mantissa
        "e[0-9]",                  // positive exponent must be signed
        "e[+-]0",                  // exponent must not have leading zeros
        "e-[1-6]$",                // not tiny enough for exponential notation
        "e+(.|1.|20)$",            // not big enough for exponential notation
        "^-?0\\.0000000",          // too tiny, should use exponential notation
        "^-?[0-9]{22}",            // too big, should use exponential notation
        "[1-9][0-9]{16}[1-9]",     // too many significant digits in integer
        "[1-9][0-9.]{17}[1-9]",    // too many significant digits in decimal
        // below here for float32 only
        "[1-9][0-9]{8}[1-9]",   // too many significant digits in integer
        "[1-9][0-9.]{9}[1-9]",  // too many significant digits in decimal
    };
    std::vector<std::regex> bad;
    for (auto* re : bad_res) bad.emplace_back(re);

    int nfail = 0;
    auto test = [&](double f, int bits) {
        std::string out;
        if (bits == 32) {
            float g = static_cast<float>(f);
            f = g;
            out = marshal_str(g);
        } else {
            out = marshal_str(f);
        }
        // The result must convert back to the same float.
        double g = 0;
        if (bits == 32) {
            float g32 = 0;
            std::from_chars(out.data(), out.data() + out.size(), g32);
            g = g32;
        } else {
            std::from_chars(out.data(), out.data() + out.size(), g);
        }
        if (f != g || std::signbit(f) != std::signbit(g)) {
            ADD_FAILURE() << "ParseFloat(" << out << "): got " << g << ", want " << f;
            nfail++;
            return;
        }
        std::size_t n = bits == 64 ? bad.size() - 2 : bad.size();
        for (std::size_t i = 0; i < n; i++) {
            if (std::regex_search(out, bad[i])) {
                ADD_FAILURE() << "Marshal(" << f << ") = " << out << "; must not match /" << bad_res[i] << "/";
                nfail++;
                return;
            }
        }
    };

    std::string digits = "1.2345678901234567890123";
    for (std::size_t i = digits.size(); i >= 2 && nfail <= 50; i--) {
        for (int exp = -30; exp <= 30; exp++) {
            for (char sign : {'+', '-'}) {
                for (int bits = 32; bits <= 64; bits += 32) {
                    std::string s = std::string(1, sign) + digits.substr(0, i) + "e" + std::to_string(exp);
                    const char* b = s.data() + (sign == '+' ? 1 : 0);  // from_chars rejects '+'
                    double f = 0;
                    if (bits == 32) {
                        float f32 = 0;
                        std::from_chars(b, s.data() + s.size(), f32);
                        f = f32;
                        test(f, bits);
                        test(std::nextafter(f32, INFINITY), bits);
                        test(std::nextafter(f32, -INFINITY), bits);
                    } else {
                        std::from_chars(b, s.data() + s.size(), f);
                        test(f, bits);
                        test(std::nextafter(f, INFINITY), bits);
                        test(std::nextafter(f, -INFINITY), bits);
                    }
                }
            }
        }
    }
    test(0, 64);
    test(std::copysign(0.0, -1.0), 64);
    test(0, 32);
    test(std::copysign(0.0, -1.0), 32);
}

TEST(GoEncodingJson, MarshalRawMessageValue) {
    struct T1 {
        [[=json::tag(",omitempty")]] json::RawMessage M;
    };
    struct T2 {
        [[=json::tag(",omitempty")]] std::shared_ptr<json::RawMessage> M;
    };
    struct S1 {
        json::RawMessage M;
    };
    json::RawMessage raw_nil{};
    json::RawMessage raw_empty{bytes::make(0)};
    json::RawMessage raw_text{to_bytes("\"foo\"")};

    // Test with nil RawMessage.
    EXPECT_EQ(marshal_str(raw_nil), "null");
    EXPECT_EQ(marshal_str(std::vector<json::RawMessage>{raw_nil}), "[null]");
    EXPECT_EQ(marshal_str(S1{raw_nil}), R"({"M":null})");
    EXPECT_EQ(marshal_str(std::map<string, json::RawMessage>{{"M", raw_nil}}), R"({"M":null})");
    EXPECT_EQ(marshal_str(T1{raw_nil}), "{}");
    EXPECT_EQ(marshal_str(T2{std::make_shared<json::RawMessage>(raw_nil)}), R"({"M":null})");

    // Test with empty, but non-nil, RawMessage.
    EXPECT_TRUE(json::Marshal(raw_empty).second);
    EXPECT_TRUE(json::Marshal(std::vector<json::RawMessage>{raw_empty}).second);
    EXPECT_TRUE(json::Marshal(S1{raw_empty}).second);
    EXPECT_EQ(marshal_str(T1{raw_empty}), "{}");
    EXPECT_TRUE(json::Marshal(T2{std::make_shared<json::RawMessage>(raw_empty)}).second);

    // Test with RawMessage with some text.
    EXPECT_EQ(marshal_str(raw_text), "\"foo\"");
    EXPECT_EQ(marshal_str(std::vector<json::RawMessage>{raw_text}), "[\"foo\"]");
    EXPECT_EQ(marshal_str(S1{raw_text}), R"({"M":"foo"})");
    EXPECT_EQ(marshal_str(T1{raw_text}), R"({"M":"foo"})");
    EXPECT_EQ(marshal_str(T2{std::make_shared<json::RawMessage>(raw_text)}), R"({"M":"foo"})");
}

TEST(GoEncodingJson, MarshalUncommonFieldNames) {
    struct {
        int A0, À, Aβ;
    } v{};
    EXPECT_EQ(marshal_str(v), "{\"A0\":0,\"\xc3\x80\":0,\"A\xce\xb2\":0}");
}

TEST(GoEncodingJson, MarshalerError) {
    const char* err_text = "json: test error";
    json::MarshalerError e1;
    e1.type = "string";
    e1.Err = errors::New(err_text);
    EXPECT_EQ(e1.Error(), "json: error calling MarshalJSON for type string: json: test error");
    json::MarshalerError e2;
    e2.type = "string";
    e2.Err = errors::New(err_text);
    e2.source_func = "TestMarshalerError";
    EXPECT_EQ(e2.Error(), "json: error calling TestMarshalerError for type string: json: test error");
}

// ---------------------------------------------------------------------------
// stream_test.Go

namespace {

// streamTest; "K" is U+212A KELVIN SIGN, which sorts after "ß".
std::vector<json::Value> stream_test() {
    json::Value::object m;
    m[string("\xe2\x84\xaa")] = "Kelvin";
    m[string("\xc3\x9f")] = "long s";
    return {0.1, "hello", nullptr, true, false, json::Value::array{"a", "b", "c"}, m, 3.14};
}

const std::string stream_encoded =
    "0.1\n\"hello\"\nnull\ntrue\nfalse\n[\"a\",\"b\",\"c\"]\n{\"\xc3\x9f\":\"long s\",\"\xe2\x84\xaa\":\"Kelvin\"}\n3.14\n";

}  // namespace

TEST(GoEncodingJson, Encoder) {
    auto values = stream_test();
    for (std::size_t i = 0; i <= values.size(); i++) {
        strings::Builder buf;
        auto enc = json::NewEncoder(buf);
        enc.SetIndent(">", ".");
        enc.SetIndent("", "");
        for (std::size_t j = 0; j < i; j++) ASSERT_FALSE(enc.Encode(values[j])) << i << "." << j;
        EXPECT_EQ(buf.view(), nlines(stream_encoded, static_cast<int>(i))) << "encoding " << i << " items";
    }
}

TEST(GoEncodingJson, EncoderIndent) {
    const std::string want =
        "0.1\n\"hello\"\nnull\ntrue\nfalse\n[\n>.\"a\",\n>.\"b\",\n>.\"c\"\n>]\n{\n>.\"\xc3\x9f\": \"long s\",\n"
        ">.\"\xe2\x84\xaa\": \"Kelvin\"\n>}\n3.14\n";
    strings::Builder buf;
    auto enc = json::NewEncoder(buf);
    enc.SetIndent(">", ".");
    for (auto& v : stream_test()) enc.Encode(v);
    EXPECT_EQ(buf.view(), want);
}

namespace {

struct strMarshaler {
    string s;
    std::pair<bytes, error> MarshalJSON() const { return {to_bytes(s), nullptr}; }
};

// Go's type C, whose MarshalJSON returns `"<&>"`.
struct C {
    std::pair<bytes, error> MarshalJSON() const { return {to_bytes("\"<&>\""), nullptr}; }
};

}  // namespace

TEST(GoEncodingJson, EncoderSetEscapeHTML) {
    struct {
        [[=json::tag("<>&#! ")]] int Valid;
        [[=json::tag("\\")]] int Invalid;
    } tag_struct{};
    struct {
        strMarshaler NonPtr;
        std::shared_ptr<strMarshaler> Ptr;
    } marshaler_struct{{"\"<str>\""}, std::make_shared<strMarshaler>(strMarshaler{"\"<str>\""})};
    struct {
        [[=json::tag("bar,string")]] string Bar;
    } string_option{"<html>foobar</html>"};

    auto check = [](const auto& v, std::string_view name, std::string_view want_escape, std::string_view want) {
        strings::Builder buf;
        auto enc = json::NewEncoder(buf);
        ASSERT_FALSE(enc.Encode(v)) << name;
        std::string got(buf.view());
        while (!got.empty() && got.back() == '\n') got.pop_back();
        EXPECT_EQ(got, want_escape) << name;
        buf.reset();
        enc.SetEscapeHTML(false);
        ASSERT_FALSE(enc.Encode(v)) << name;
        got = std::string(buf.view());
        while (!got.empty() && got.back() == '\n') got.pop_back();
        EXPECT_EQ(got, want) << name << " with set_escape_html(false)";
    };
    check(C{}, "c", "\"\\u003c\\u0026\\u003e\"", "\"<&>\"");
    check(std::string("<&>"), "\"<&>\"", "\"\\u003c\\u0026\\u003e\"", "\"<&>\"");
    check(tag_struct, "tagStruct", "{\"\\u003c\\u003e\\u0026#! \":0,\"Invalid\":0}", "{\"<>&#! \":0,\"Invalid\":0}");
    check(marshaler_struct, "\"<str>\"", "{\"NonPtr\":\"\\u003cstr\\u003e\",\"Ptr\":\"\\u003cstr\\u003e\"}",
          "{\"NonPtr\":\"<str>\",\"Ptr\":\"<str>\"}");
    check(string_option, "stringOption", "{\"bar\":\"\\\"\\\\u003chtml\\\\u003efoobar\\\\u003c/html\\\\u003e\\\"\"}",
          "{\"bar\":\"\\\"<html>foobar</html>\\\"\"}");
}

TEST(GoEncodingJson, Decoder) {
    auto values = stream_test();
    for (std::size_t i = 0; i <= values.size(); i++) {
        std::string buf;
        for (char c : nlines(stream_encoded, static_cast<int>(i))) {
            if (c != '\n') buf.push_back(c);
        }
        io::StringReader r{string(buf)};
        auto dec = json::NewDecoder(r);
        std::vector<json::Value> out(i);
        for (std::size_t j = 0; j < i; j++) ASSERT_FALSE(dec.Decode(&out[j])) << "decode #" << j << "/" << i;
        EXPECT_TRUE(std::equal(out.begin(), out.end(), values.begin())) << "decoding " << i << " items";
    }
}

TEST(GoEncodingJson, RawMessage) {
    struct {
        double X;
        json::RawMessage Id;
        float Y;
    } data{};
    const std::string raw = "[\"\\u0056\",null]";
    const std::string want = "{\"X\":0.1,\"Id\":[\"\\u0056\",null],\"Y\":0.2}";
    ASSERT_FALSE(json::Unmarshal(want, &data));
    EXPECT_EQ(as_string_view(data.Id.data), raw);
    EXPECT_EQ(marshal_str(data), want);
}

TEST(GoEncodingJson, NullRawMessage) {
    struct {
        double X;
        json::RawMessage Id;
        std::shared_ptr<json::RawMessage> IdPtr;
        float Y;
    } data{};
    const std::string want = R"({"X":0.1,"Id":null,"IdPtr":null,"Y":0.2})";
    ASSERT_FALSE(json::Unmarshal(want, &data));
    EXPECT_EQ(as_string_view(data.Id.data), "null");
    EXPECT_FALSE(data.IdPtr);
    EXPECT_EQ(marshal_str(data), want);
}

// ---------------------------------------------------------------------------
// decode_test.Go

TEST(GoEncodingJson, Escape) {
    const std::string input = "\"foobar\"<html> [\xe2\x80\xa8 \xe2\x80\xa9]";
    const std::string want = "\"\\\"foobar\\\"\\u003chtml\\u003e [\\u2028 \\u2029]\"";
    EXPECT_EQ(marshal_str(input), want);
}

TEST(GoEncodingJson, EmptyString) {
    struct T2 {
        [[=json::tag(",string")]] int Number1;
        [[=json::tag(",string")]] int Number2;
    };
    io::StringReader r(R"({"Number1":"1", "Number2":""})");
    auto dec = json::NewDecoder(r);
    T2 got{};
    EXPECT_TRUE(dec.Decode(&got));
    EXPECT_EQ(got.Number1, 1);
}

TEST(GoEncodingJson, NullString) {
    // A null for ,string is not replaced with the previous quoted string
    // (issue 7046), and is not an error (issue 2540, issue 8587).
    struct T {
        [[=json::tag(",string")]] int A;
        [[=json::tag(",string")]] int B;
        [[=json::tag(",string")]] std::shared_ptr<int> C;
    };
    T s{};
    s.B = 1;
    s.C = std::make_shared<int>(2);
    ASSERT_FALSE(json::Unmarshal(R"({"A": "1", "B": null, "C": null})", &s));
    EXPECT_EQ(s.B, 1);
    EXPECT_FALSE(s.C);
}

TEST(GoEncodingJson, UnmarshalTypeError) {
    string s;
    auto err = json::Unmarshal(R"({"user": "name"})", &s);  // issue 4628.
    EXPECT_TRUE(errors::As<json::UnmarshalTypeError>(err)) << err;
}

TEST(GoEncodingJson, UnmarshalSyntax) {
    json::Value x;
    for (std::string in : {"tru", "fals", "nul", "123e", "\"hello", "[1,2,3", "{\"key\":1", "{\"key\":1,"}) {
        auto err = json::Unmarshal(in, &x);
        EXPECT_TRUE(errors::As<json::SyntaxError>(err)) << in << ": " << err;
    }
}

TEST(GoEncodingJson, SkipArrayObjects) {
    std::array<json::Value, 0> dest;
    EXPECT_FALSE(json::Unmarshal("[{}]", &dest));
}

TEST(GoEncodingJson, Prefilled) {
    // Semantics of pre-filled data: struct fields, map elements, slices,
    // and arrays. Issues 4900 and 8837, among others.
    struct XYZ {
        json::Value X, Y, Z;
    };
    XYZ xyz{3, 4, 1.5};
    ASSERT_FALSE(json::Unmarshal(R"({"X": 1, "Y": 2})", &xyz));
    EXPECT_TRUE(xyz.X == json::Value(1) && xyz.Y == json::Value(2) && xyz.Z == json::Value(1.5));

    std::map<string, json::Value> m{{"X", 3}, {"Y", 4}, {"Z", 1.5}};
    ASSERT_FALSE(json::Unmarshal(R"({"X": 1, "Y": 2})", &m));
    EXPECT_TRUE(m["X"] == json::Value(1) && m["Y"] == json::Value(2) && m["Z"] == json::Value(1.5));

    std::vector<int> s1{1};
    ASSERT_FALSE(json::Unmarshal("[2]", &s1));
    EXPECT_EQ(s1, std::vector<int>{2});
    std::vector<int> s2{1};
    ASSERT_FALSE(json::Unmarshal("[2, 3]", &s2));
    EXPECT_EQ(s2, (std::vector<int>{2, 3}));
    std::array<int, 1> a1{1};
    ASSERT_FALSE(json::Unmarshal("[2, 3]", &a1));
    EXPECT_EQ(a1, (std::array<int, 1>{2}));
    std::array<int, 2> a2{1, 2};
    ASSERT_FALSE(json::Unmarshal("[3]", &a2));
    EXPECT_EQ(a2, (std::array<int, 2>{3, 0}));

    double pi = std::numbers::pi;
    auto err = json::Unmarshal("1e1000", &pi);
    EXPECT_EQ(pi, std::numbers::pi);
    auto* te = errors::As<json::UnmarshalTypeError>(err);
    ASSERT_TRUE(te) << err;
    EXPECT_EQ(te->value, "number 1e1000");
    EXPECT_EQ(te->type, "double");
}

TEST(GoEncodingJson, InvalidUnmarshal) {
    auto err = json::Unmarshal(R"({"a":"1"})", static_cast<int*>(nullptr));
    ASSERT_TRUE(err);
    EXPECT_TRUE(errors::As<json::InvalidUnmarshalError>(err));
    EXPECT_EQ(err.Error(), "json: Unmarshal(nil *int)");
}

TEST(GoEncodingJson, LargeByteSlice) {
    bytes s0 = bytes::make(2000);
    for (std::size_t i = 0; i < s0.len(); i++) s0[i] = static_cast<byte>(i);
    auto [b, err] = json::Marshal(s0);
    ASSERT_FALSE(err);
    bytes s1;
    ASSERT_FALSE(json::Unmarshal(b, &s1));
    EXPECT_TRUE(std::equal(s0.begin(), s0.end(), s1.begin(), s1.end()));
}

namespace {

struct unmarshaler {
    error UnmarshalJSON(std::span<const byte>) { return nullptr; }
};

}  // namespace

TEST(GoEncodingJson, UnmarshalMaxDepth) {
    auto repeat = [](std::string_view s, int n) { return strings::Repeat(s, static_cast<std::size_t>(n)).str(); };
    struct {
        std::string name, data;
        bool err_max_depth;
    } tests[] = {
        {"ArrayUnderMaxNestingDepth", R"({"a":)" + repeat("[", 10000 - 1) + repeat("]", 10000 - 1) + "}", false},
        {"ArrayOverMaxNestingDepth", R"({"a":)" + repeat("[", 10000) + repeat("]", 10000) + "}", true},
        {"ArrayOverStackDepth", R"({"a":)" + repeat("[", 3000000) + repeat("]", 3000000) + "}", true},
        {"ObjectUnderMaxNestingDepth", R"({"a":)" + repeat(R"({"a":)", 10000 - 1) + "0" + repeat("}", 10000 - 1) + "}", false},
        {"ObjectOverMaxNestingDepth", R"({"a":)" + repeat(R"({"a":)", 10000) + "0" + repeat("}", 10000) + "}", true},
        {"ObjectOverStackDepth", R"({"a":)" + repeat(R"({"a":)", 3000000) + "0" + repeat("}", 3000000) + "}", true},
    };
    struct typed_named {
        [[=json::tag("a")]] json::Value A;
    };
    struct typed_missing {
        [[=json::tag("b")]] json::Value B;
    };
    for (auto& tt : tests) {
        auto check = [&](error err, std::string_view target) {
            if (!tt.err_max_depth) {
                EXPECT_FALSE(err) << tt.name << ": " << target << ": " << err;
            } else {
                EXPECT_TRUE(err && strings::Contains(err.Error(), "exceeded max depth")) << tt.name << ": " << target << ": " << err;
            }
        };
        json::Value unstructured;
        check(json::Unmarshal(tt.data, &unstructured), "unstructured");
        typed_named named{};
        check(json::Unmarshal(tt.data, &named), "typed named field");
        typed_missing missing{};
        check(json::Unmarshal(tt.data, &missing), "typed missing field");
        unmarshaler custom;
        check(json::Unmarshal(tt.data, &custom), "custom unmarshaler");
    }
}
