// Ported from Go 1.27.3's mime/multipart/multipart_test.go, formdata_test.go
// and writer_test.go, with Go's test names. Left out: TestBoundaryLine (of an
// unexported method), the tests of GODEBUG settings, which go++ doesn't
// have, and the benchmarks.
#include "go_multipart_testdata.hpp"

#include <gopp/bytes.hpp>
#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/mime.hpp>
#include <gopp/mime/multipart.hpp>
#include <gopp/net/textproto.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using namespace gopp;
using textproto::MIMEHeader;

namespace {

std::string repeat(std::string_view s, std::size_t n) {
    std::string r;
    r.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; i++) r += s;
    return r;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    std::string r;
    std::size_t i = 0;
    for (;;) {
        auto j = s.find(from, i);
        if (j == std::string::npos) break;
        r.append(s, i, j - i);
        r += to;
        i = j + from.size();
    }
    r.append(s, i);
    return r;
}

std::string crlf(std::string s) { return replaceAll(std::move(s), "\n", "\r\n"); }

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

TEST(GoMultipart, NameAccessors) {
    std::vector<std::array<std::string, 3>> tests = {
        {R"(form-data; name="foo")", "foo", ""},
        {R"( form-data ; name=foo)", "foo", ""},
        {R"(FORM-DATA;name="foo")", "foo", ""},
        {R"( FORM-DATA ; name="foo")", "foo", ""},
        {R"( FORM-DATA ; name="foo")", "foo", ""},
        {R"( FORM-DATA ; name=foo)", "foo", ""},
        {R"( FORM-DATA ; filename="foo.txt"; name=foo; baz=quux)", "foo", "foo.txt"},
        {R"( not-form-data ; filename="bar.txt"; name=foo; baz=quux)", "", "bar.txt"},
    };
    int i = 0;
    for (const auto& test : tests) {
        MIMEHeader h;
        h.Set("Content-Disposition", test[0]);
        auto p = make_ptr<multipart::Part>(multipart::NewReader(strings::NewReader(""), "x"), 0, h, true);
        EXPECT_EQ(p->FormName(), test[1]) << "test " << i;
        EXPECT_EQ(p->FileName(), test[2]) << "test " << i;
        i++;
    }
}

const std::string longLine = repeat(std::string("\n\n\r\r\r\n\r\0", 8), (1 << 20) / 8);

std::string testMultipartBody(std::string_view sep) {
    std::string testBody = R"(
This is a multi-part message.  This line is ignored.
--MyBoundary
Header1: value1
HEADER2: value2
foo-bar: baz

My value
The end.
--MyBoundary
name: bigsection

[longline]
--MyBoundary
Header1: value1b
HEADER2: value2b
foo-bar: bazb

Line 1
Line 2
Line 3 ends in a newline, but just one.

--MyBoundary

never read data
--MyBoundary--


useless trailer
)";
    testBody = replaceAll(testBody, "\n", sep);
    auto i = testBody.find("[longline]");
    return testBody.replace(i, 10, longLine);
}

struct slowReader final : io::Reader {
    ptr<io::Reader> r;
    explicit slowReader(ptr<io::Reader> r_) : r(std::move(r_)) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (p.empty()) return r->Read(p);
        return r->Read(p.first(1));
    }
};

void testMultipart(ptr<io::Reader> r, bool onlyNewlines) {
    auto reader = multipart::NewReader(r, "MyBoundary");
    strings::Builder buf;

    // Part1
    auto [part, err] = reader->NextPart();
    ASSERT_TRUE(part && !err) << "Expected part1";
    EXPECT_EQ(part->Header.Get("Header1"), "value1");
    EXPECT_EQ(part->Header.Get("foo-bar"), "baz");
    EXPECT_EQ(part->Header.Get("Foo-Bar"), "baz");
    buf.Reset();
    EXPECT_FALSE(io::Copy(buf, *part).second) << "part 1 copy";

    auto adjustNewlines = [&](std::string s) { return onlyNewlines ? replaceAll(s, "\r\n", "\n") : s; };
    EXPECT_EQ(buf.String(), adjustNewlines("My value\r\nThe end.")) << "Value of first part";

    // Part2
    std::tie(part, err) = reader->NextPart();
    ASSERT_FALSE(err) << "Expected part2; got: " << err;
    EXPECT_EQ(part->Header.Get("name"), "bigsection");
    buf.Reset();
    EXPECT_FALSE(io::Copy(buf, *part).second) << "part 2 copy";
    EXPECT_EQ(buf.len(), longLine.size());
    EXPECT_TRUE(buf.String() == longLine) << "part2 long body didn't match";

    // Part3
    std::tie(part, err) = reader->NextPart();
    ASSERT_TRUE(part && !err) << "Expected part3";
    EXPECT_EQ(part->Header.Get("foo-bar"), "bazb");
    buf.Reset();
    EXPECT_FALSE(io::Copy(buf, *part).second) << "part 3 copy";
    EXPECT_EQ(buf.String(), adjustNewlines("Line 1\r\nLine 2\r\nLine 3 ends in a newline, but just one.\r\n"));

    // Part4
    std::tie(part, err) = reader->NextPart();
    ASSERT_TRUE(part && !err) << "Expected part 4 without errors";

    // Non-existent part5
    std::tie(part, err) = reader->NextPart();
    EXPECT_FALSE(part) << "Didn't expect a fifth part.";
    EXPECT_EQ(err, io::EOF_) << "On fifth part expected io.EOF; got " << err;
}

TEST(GoMultipart, Multipart) { testMultipart(strings::NewReader(testMultipartBody("\r\n")), false); }

TEST(GoMultipart, MultipartOnlyNewlines) { testMultipart(strings::NewReader(testMultipartBody("\n")), true); }

TEST(GoMultipart, MultipartSlowInput) {
    testMultipart(make_ptr<slowReader>(strings::NewReader(testMultipartBody("\r\n"))), false);
}

TEST(GoMultipart, VariousTextLineEndings) {
    std::vector<std::string> tests = {
        "Foo\nBar", "Foo\nBar\n", "Foo\r\nBar", "Foo\r\nBar\r\n", "Foo\rBar", "Foo\rBar\r",
        std::string("\x00\x01\x02\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 11),
    };
    int testNum = 0;
    for (const auto& expectedBody : tests) {
        std::string body = "--BOUNDARY\r\nContent-Disposition: form-data; name=\"value\"\r\n\r\n" + expectedBody +
                           "\r\n--BOUNDARY--\r\n";
        auto reader = multipart::NewReader(strings::NewReader(body), "BOUNDARY");
        bytes::Buffer buf;
        auto [part, err] = reader->NextPart();
        if (!part) {
            ADD_FAILURE() << "Expected a body part on text " << testNum;
            continue;
        }
        if (err) {
            ADD_FAILURE() << "Unexpected error on text " << testNum << ": " << err;
            continue;
        }
        auto [written, cerr] = io::Copy(buf, *part);
        EXPECT_EQ(buf.String(), expectedBody) << "test " << testNum;
        EXPECT_FALSE(cerr) << "Error copying multipart; bytes=" << written;
        std::tie(part, err) = reader->NextPart();
        EXPECT_FALSE(part) << "Unexpected part in test " << testNum;
        EXPECT_EQ(err, io::EOF_) << "On test " << testNum;
        testNum++;
    }
}

constexpr int maxReadThreshold = 1 << 20;

struct maliciousReader final : io::Reader {
    int n = 0;
    std::pair<isize, error> Read(std::span<byte> b) override {
        n += int(b.size());
        if (n >= maxReadThreshold) {
            ADD_FAILURE() << "too much was read";
            return {0, io::EOF_};
        }
        return {isize(b.size()), nullptr};
    }
};

TEST(GoMultipart, LineLimit) {
    auto mr = make_ptr<maliciousReader>();
    auto r = multipart::NewReader(mr, "fooBoundary");
    auto [part, err] = r->NextPart();
    EXPECT_FALSE(part) << "unexpected part read";
    EXPECT_TRUE(err) << "expected an error";
    EXPECT_LT(mr->n, maxReadThreshold);
}

TEST(GoMultipart, MultipartTruncated) {
    for (std::string body : {
             std::string("\nThis is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo-bar: baz\n\nOh no, "
                         "premature EOF!\n"),
             std::string("\nThis is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo-bar: baz\n\nOh no, "
                         "premature EOF!\n--MyBoundary-"),
         }) {
        body = crlf(body);
        auto r = multipart::NewReader(strings::NewReader(body), "MyBoundary");
        auto [part, err] = r->NextPart();
        ASSERT_FALSE(err) << "didn't get a part";
        auto [_, cerr] = io::Copy(io::Discard(), *part);
        ASSERT_EQ(cerr, io::ErrUnexpectedEOF) << cerr;
    }
}

struct sentinelReader final : io::Reader {
    chan<empty> done;  // closed when this reader is read from
    explicit sentinelReader(chan<empty> d) : done(std::move(d)) {}
    std::pair<isize, error> Read(std::span<byte>) override {
        if (done) {
            done.close();
            done = nullptr;
        }
        return {0, io::EOF_};
    }
};

// TestMultipartStreamReadahead tests that PartReader does not block on
// reading past the end of a part, ensuring that it can be used on a stream
// like multipart/x-mixed-replace. See golang.org/issue/15431.
TEST(GoMultipart, MultipartStreamReadahead) {
    std::string testBody1 = crlf("\nThis is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo-bar: baz\n\nBody\n--MyBoundary\n");
    std::string testBody2 = crlf("foo-bar: bop\n\nBody 2\n--MyBoundary--\n");
    auto done1 = make_chan<empty>();
    auto reader = multipart::NewReader(
        io::MultiReader({strings::NewReader(testBody1), make_ptr<sentinelReader>(done1), strings::NewReader(testBody2)}),
        "MyBoundary");
    int i = 0;
    auto readPart = [&](MIMEHeader hdr, std::string body) {
        auto [part, err] = reader->NextPart();
        ASSERT_TRUE(part && !err) << "Part " << i << ": NextPart failed: " << err;
        EXPECT_EQ(part->Header, hdr) << "Part " << i;
        auto [data, rerr] = io::ReadAll(*part);
        EXPECT_EQ(str(data), body) << "Part " << i << " body";
        ASSERT_FALSE(rerr) << "Part " << i << ": ReadAll failed: " << rerr;
        i++;
    };
    readPart({{"Foo-Bar", {"baz"}}}, "Body");
    bool readPast = false;
    select(recv(done1, [&](empty) { readPast = true; }), defaultCase([] {}));
    EXPECT_FALSE(readPast) << "Reader read past second boundary";
    readPart({{"Foo-Bar", {"bop"}}}, "Body 2");
}

TEST(GoMultipart, LineContinuation) {
    // This body, extracted from an email, contains headers that span
    // multiple lines.
    std::string testBody =
        "\n--Apple-Mail-2-292336769\nContent-Transfer-Encoding: 7bit\nContent-Type: text/plain;\n\tcharset=US-ASCII;\n"
        "\tdelsp=yes;\n\tformat=flowed\n\nI'm finding the same thing happening on my system (10.4.1).\n\n\n"
        "--Apple-Mail-2-292336769\nContent-Transfer-Encoding: quoted-printable\nContent-Type: text/html;\n"
        "\tcharset=ISO-8859-1\n\n<HTML><BODY>I'm finding the same thing =\nhappening on my system (10.4.1).=A0 But I "
        "built it with XCode =\n2.0.</BODY></=\nHTML>=\n\r\n--Apple-Mail-2-292336769--\n";
    auto r = multipart::NewReader(strings::NewReader(testBody), "Apple-Mail-2-292336769");
    for (int i = 0; i < 2; i++) {
        auto [part, err] = r->NextPart();
        ASSERT_FALSE(err) << "didn't get a part";
        strings::Builder buf;
        auto [n, cerr] = io::Copy(buf, *part);
        EXPECT_FALSE(cerr) << "error reading part: " << cerr << "\nread so far: " << buf.String();
        EXPECT_GT(n, 0);
    }
}

void testQuotedPrintableEncoding(const std::string& cte) {
    // From https://golang.org/issue/4411
    std::string body =
        "--0016e68ee29c5d515f04cedf6733\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: "
        "form-data; name=text\r\nContent-Transfer-Encoding: " +
        cte +
        "\r\n\r\nwords words words words words words words words words words words words wor=\r\nds words words words "
        "words words words words words words words words words =\r\nwords words words words words words words words "
        "words words words words wor=\r\nds words words words words words words words words words words words words "
        "=\r\nwords words words words words words words words words\r\n--0016e68ee29c5d515f04cedf6733\r\nContent-Type: "
        "text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; "
        "name=submit\r\n\r\nSubmit\r\n--0016e68ee29c5d515f04cedf6733--";
    auto r = multipart::NewReader(strings::NewReader(body), "0016e68ee29c5d515f04cedf6733");
    auto [part, err] = r->NextPart();
    ASSERT_FALSE(err) << err;
    EXPECT_FALSE(part->Header.contains("Content-Transfer-Encoding")) << "unexpected Content-Transfer-Encoding";
    strings::Builder buf;
    EXPECT_FALSE(io::Copy(buf, *part).second);
    EXPECT_EQ(buf.String(), repeat("words ", 58) + "words");
}

TEST(GoMultipart, QuotedPrintableEncoding) {
    for (std::string cte : {"quoted-printable", "Quoted-PRINTABLE"}) {
        SCOPED_TRACE(cte);
        testQuotedPrintableEncoding(cte);
    }
}

TEST(GoMultipart, RawPart) {
    // https://github.com/golang/go/issues/29090
    std::string body = crlf(R"(--0016e68ee29c5d515f04cedf6733
Content-Type: text/plain; charset="utf-8"
Content-Transfer-Encoding: quoted-printable

<div dir=3D"ltr">Hello World.</div>
--0016e68ee29c5d515f04cedf6733
Content-Type: text/plain; charset="utf-8"
Content-Transfer-Encoding: quoted-printable

<div dir=3D"ltr">Hello World.</div>
--0016e68ee29c5d515f04cedf6733--)");
    auto r = multipart::NewReader(strings::NewReader(body), "0016e68ee29c5d515f04cedf6733");

    // This part is expected to be raw, bypassing the automatic handling of
    // quoted-printable.
    auto [part, err] = r->NextRawPart();
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(part->Header.contains("Content-Transfer-Encoding")) << "missing Content-Transfer-Encoding";
    strings::Builder buf;
    EXPECT_FALSE(io::Copy(buf, *part).second);
    // Data is still quoted-printable.
    EXPECT_EQ(buf.String(), R"(<div dir=3D"ltr">Hello World.</div>)");

    // This part is expected to have automatic decoding of quoted-printable.
    std::tie(part, err) = r->NextPart();
    ASSERT_FALSE(err) << err;
    EXPECT_FALSE(part->Header.contains("Content-Transfer-Encoding")) << "unexpected Content-Transfer-Encoding";
    buf.Reset();
    EXPECT_FALSE(io::Copy(buf, *part).second);
    // QP data has been decoded.
    EXPECT_EQ(buf.String(), R"(<div dir="ltr">Hello World.</div>)");
}

// Test parsing an image attachment from gmail, which previously failed.
TEST(GoMultipart, Nested) {
    // nested-mime is the body part of a multipart/mixed email with boundary
    // e89a8ff1c1e83553e304be640612.
    auto mr = multipart::NewReader(strings::NewReader(string(nestedMime)), "e89a8ff1c1e83553e304be640612");
    auto [p, err] = mr->NextPart();
    ASSERT_FALSE(err) << "error reading first section (alternative): " << err;

    // Read the inner text/plain and text/html sections of the
    // multipart/alternative.
    auto mr2 = multipart::NewReader(p, "e89a8ff1c1e83553e004be640610");
    auto [p2, err2] = mr2->NextPart();
    ASSERT_FALSE(err2) << "reading text/plain part: " << err2;
    {
        auto [b, rerr] = io::ReadAll(*p2);
        ASSERT_EQ(str(b), "*body*\r\n");
        ASSERT_FALSE(rerr);
    }
    std::tie(p2, err2) = mr2->NextPart();
    ASSERT_FALSE(err2) << "reading text/html part: " << err2;
    {
        auto [b, rerr] = io::ReadAll(*p2);
        ASSERT_EQ(str(b), "<b>body</b>\r\n");
        ASSERT_FALSE(rerr);
    }
    std::tie(p2, err2) = mr2->NextPart();
    ASSERT_EQ(err2, io::EOF_) << "final inner NextPart = " << err2;

    // Back to the outer multipart/mixed, reading the image attachment.
    std::tie(p, err) = mr->NextPart();
    ASSERT_FALSE(err) << "error reading the image attachment at the end: " << err;
    std::tie(p, err) = mr->NextPart();
    ASSERT_EQ(err, io::EOF_) << "final outer NextPart = " << err;
}

struct headerBody {
    MIMEHeader header;
    std::string body;
    bool operator==(const headerBody&) const = default;
};

std::ostream& operator<<(std::ostream& os, const headerBody& hb) {
    os << "{";
    for (const auto& [k, vs] : hb.header) {
        os << k << ":";
        for (const auto& v : vs) os << " " << strconv::Quote(v);
        os << "; ";
    }
    return os << strconv::Quote(hb.body) << "}";
}

headerBody formData(std::string key, std::string value) {
    return {{{"Content-Type", {"text/plain; charset=ISO-8859-1"}}, {"Content-Disposition", {"form-data; name=" + key}}},
            std::move(value)};
}

struct parseTest {
    std::string name, in, sep;
    std::vector<headerBody> want;
};

constexpr std::size_t peekBufferSize = 4096;

parseTest roundTripParseTest() {
    parseTest t{"round trip", "", "",
                {formData("empty", ""), formData("lf", "\n"), formData("cr", "\r"), formData("crlf", "\r\n"),
                 formData("foo", "bar")}};
    auto buf = make_ptr<strings::Builder>();
    auto w = multipart::NewWriter(buf);
    for (const auto& p : t.want) {
        auto [pw, err] = w->CreatePart(p.header);
        if (err) panic(err.Error().view());
        if (auto [_, werr] = io::WriteString(*pw, p.body); werr) panic(werr.Error().view());
    }
    w->Close();
    t.in = buf->String().str();
    t.sep = w->Boundary().str();
    return t;
}

std::vector<parseTest> parseTests() {
    const std::string safeCountSep = "08b84578eabc563dcba967a945cdf0d9f613864a8f4a716f0e81caa71a74";
    const std::size_t safeCount = peekBufferSize - (3 + safeCountSep.size() + 1 + 1);
    return {
        // Actual body from App Engine on a blob upload. The final part (the
        // Content-Type: message/external-body) is what App Engine replaces
        // the uploaded file with. The other form fields (prefixed with
        // "other" in their form-data name) are unchanged. A bug was reported
        // with blob uploads failing when the other fields were empty. This
        // was the MIME POST body that previously failed.
        {"App Engine post", "--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherEmpty1\r\n\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherFoo1\r\n\r\nfoo\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherFoo2\r\n\r\nfoo\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherEmpty2\r\n\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherRepeatFoo\r\n\r\nfoo\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherRepeatFoo\r\n\r\nfoo\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherRepeatEmpty\r\n\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=otherRepeatEmpty\r\n\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=submit\r\n\r\nSubmit\r\n--00151757727e9583fd04bfbca4c6\r\nContent-Type: message/external-body; charset=ISO-8859-1; blob-key=AHAZQqG84qllx7HUqO_oou5EvdYQNS3Mbbkb0RjjBoM_Kc1UqEN2ygDxWiyCPulIhpHRPx-VbpB6RX4MrsqhWAi_ZxJ48O9P2cTIACbvATHvg7IgbvZytyGMpL7xO1tlIvgwcM47JNfv_tGhy1XwyEUO8oldjPqg5Q\r\nContent-Disposition: form-data; name=file; filename=\"fall.png\"\r\n\r\nContent-Type: image/png\r\nContent-Length: 232303\r\nX-AppEngine-Upload-Creation: 2012-05-10 23:14:02.715173\r\nContent-MD5: MzRjODU1ZDZhZGU1NmRlOWEwZmMwMDdlODBmZTA0NzA=\r\nContent-Disposition: form-data; name=file; filename=\"fall.png\"\r\n\r\n\r\n--00151757727e9583fd04bfbca4c6--",
         "00151757727e9583fd04bfbca4c6",
         {
             formData("otherEmpty1", ""), formData("otherFoo1", "foo"), formData("otherFoo2", "foo"),
             formData("otherEmpty2", ""), formData("otherRepeatFoo", "foo"), formData("otherRepeatFoo", "foo"),
             formData("otherRepeatEmpty", ""), formData("otherRepeatEmpty", ""), formData("submit", "Submit"),
             {{{"Content-Type", {"message/external-body; charset=ISO-8859-1; blob-key=AHAZQqG84qllx7HUqO_oou5EvdYQNS3Mbbkb0RjjBoM_Kc1UqEN2ygDxWiyCPulIhpHRPx-VbpB6RX4MrsqhWAi_ZxJ48O9P2cTIACbvATHvg7IgbvZytyGMpL7xO1tlIvgwcM47JNfv_tGhy1XwyEUO8oldjPqg5Q"}},
               {"Content-Disposition", {"form-data; name=file; filename=\"fall.png\""}}},
              "Content-Type: image/png\r\nContent-Length: 232303\r\nX-AppEngine-Upload-Creation: 2012-05-10 23:14:02.715173\r\nContent-MD5: MzRjODU1ZDZhZGU1NmRlOWEwZmMwMDdlODBmZTA0NzA=\r\nContent-Disposition: form-data; name=file; filename=\"fall.png\"\r\n\r\n"},
         }},

        // Single empty part, ended with --boundary immediately after headers.
        {"single empty part, --boundary", "--abc\r\nFoo: bar\r\n\r\n--abc--", "abc", {{{{"Foo", {"bar"}}}, ""}}},

        // Single empty part, ended with \r\n--boundary immediately after
        // headers.
        {"single empty part, \r\n--boundary", "--abc\r\nFoo: bar\r\n\r\n\r\n--abc--", "abc", {{{{"Foo", {"bar"}}}, ""}}},

        // Final part empty.
        {"final part empty", "--abc\r\nFoo: bar\r\n\r\n--abc\r\nFoo2: bar2\r\n\r\n--abc--", "abc",
         {{{{"Foo", {"bar"}}}, ""}, {{{"Foo2", {"bar2"}}}, ""}}},

        // Final part empty with newlines after final separator.
        {"final part empty then crlf", "--abc\r\nFoo: bar\r\n\r\n--abc--\r\n", "abc", {{{{"Foo", {"bar"}}}, ""}}},

        // Final part empty with lwsp-chars after final separator.
        {"final part empty then lwsp", "--abc\r\nFoo: bar\r\n\r\n--abc-- \t", "abc", {{{{"Foo", {"bar"}}}, ""}}},

        // No parts (empty form as submitted by Chrome)
        {"no parts", "------WebKitFormBoundaryQfEAfzFOiSemeHfA--\r\n", "----WebKitFormBoundaryQfEAfzFOiSemeHfA", {}},

        // Part containing data starting with the boundary, but with
        // additional suffix.
        {"fake separator as data", "--sep\r\nFoo: bar\r\n\r\n--sepFAKE\r\n--sep--", "sep", {{{{"Foo", {"bar"}}}, "--sepFAKE"}}},

        // Part containing a boundary with whitespace following it.
        {"boundary with whitespace", "--sep \r\nFoo: bar\r\n\r\ntext\r\n--sep--", "sep", {{{{"Foo", {"bar"}}}, "text"}}},

        // With ignored leading line.
        {"leading line", crlf("This is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo: bar\n\n\n--MyBoundary--"),
         "MyBoundary", {{{{"Foo", {"bar"}}}, ""}}},

        // Issue 10616; minimal
        {"issue 10616 minimal", "--sep \r\nFoo: bar\r\n\r\na\r\n--sep_alt\r\nb\r\n\r\n--sep--", "sep",
         {{{{"Foo", {"bar"}}}, "a\r\n--sep_alt\r\nb\r\n"}}},

        // Issue 10616; full example from bug.
        {"nested separator prefix is outer separator", crlf(R"(------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9
Content-Type: multipart/alternative; boundary="----=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt"

------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt
Content-Type: text/plain; charset="utf-8"
Content-Transfer-Encoding: 8bit

This is a multi-part message in MIME format.

------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt
Content-Type: text/html; charset="utf-8"
Content-Transfer-Encoding: 8bit

html things
------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt--
------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9--)"),
         "----=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9",
         {{{{"Content-Type", {R"(multipart/alternative; boundary="----=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt")"}}},
           crlf(R"(------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt
Content-Type: text/plain; charset="utf-8"
Content-Transfer-Encoding: 8bit

This is a multi-part message in MIME format.

------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt
Content-Type: text/html; charset="utf-8"
Content-Transfer-Encoding: 8bit

html things
------=_NextPart_4c2fbafd7ec4c8bf08034fe724b608d9_alt--)")}}},

        // Issue 12662: Check that we don't consume the leading \r if the
        // peekBuffer ends in '\r\n--separator-'
        {"peek buffer boundary condition",
         crlf("--00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db\nContent-Disposition: form-data; "
              "name=\"block\"; filename=\"block\"\nContent-Type: application/octet-stream\n\n" +
              repeat("A", peekBufferSize - 65) + "\n--00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db--"),
         "00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db",
         {{{{"Content-Type", {"application/octet-stream"}}, {"Content-Disposition", {R"(form-data; name="block"; filename="block")"}}},
           repeat("A", peekBufferSize - 65)}}},

        // Issue 12662: Same test as above with \r\n at the end
        {"peek buffer boundary condition",
         crlf("--00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db\nContent-Disposition: form-data; "
              "name=\"block\"; filename=\"block\"\nContent-Type: application/octet-stream\n\n" +
              repeat("A", peekBufferSize - 65) + "\n--00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db--\n"),
         "00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db",
         {{{{"Content-Type", {"application/octet-stream"}}, {"Content-Disposition", {R"(form-data; name="block"; filename="block")"}}},
           repeat("A", peekBufferSize - 65)}}},

        // Issue 12662v2: We want to make sure that for short buffers that
        // end with '\r\n--separator-' we always consume at least one (valid)
        // symbol from the peekBuffer
        {"peek buffer boundary condition",
         crlf("--aaaaaaaaaa00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db\nContent-Disposition: form-data; "
              "name=\"block\"; filename=\"block\"\nContent-Type: application/octet-stream\n\n" +
              repeat("A", peekBufferSize) + "\n--aaaaaaaaaa00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db--"),
         "aaaaaaaaaa00ffded004d4dd0fdf945fbdef9d9050cfd6a13a821846299b27fc71b9db",
         {{{{"Content-Type", {"application/octet-stream"}}, {"Content-Disposition", {R"(form-data; name="block"; filename="block")"}}},
           repeat("A", peekBufferSize)}}},

        // Context: https://github.com/camlistore/camlistore/issues/642 If
        // the file contents in the form happens to have a size such as:
        // size = peekBufferSize - (len("\n--") + len(boundary) + len("\r") + 1),
        // (modulo peekBufferSize) then peekBufferSeparatorIndex was wrongly
        // returning (-1, false), which was leading to an nCopy cut such as:
        // "somedata\r| |\n--Boundary\r" (instead of "somedata| |\r\n--Boundary\r"),
        // which was making the subsequent Read miss the boundary.
        {"safeCount off by one",
         crlf("--" + safeCountSep + "\nContent-Disposition: form-data; name=\"myfile\"; filename=\"my-file.txt\"\nContent-Type: "
              "application/octet-stream\n\n") +
             repeat("A", safeCount) + crlf("\n--" + safeCountSep + "\nContent-Disposition: form-data; name=\"key\"\n\nval\n--" + safeCountSep + "--\n"),
         safeCountSep,
         {{{{"Content-Type", {"application/octet-stream"}}, {"Content-Disposition", {R"(form-data; name="myfile"; filename="my-file.txt")"}}},
           repeat("A", safeCount)},
          {{{"Content-Disposition", {R"(form-data; name="key")"}}}, "val"}}},

        // Issue 46042; a nested multipart uses the outer separator followed
        // by a dash.
        {"nested separator prefix is outer separator followed by a dash",
         crlf("--foo\nContent-Type: multipart/alternative; boundary=\"foo-bar\"\n\n--foo-bar\n\nBody\n--foo-bar\n\nBody2\n--foo-bar--\n--foo--"),
         "foo",
         {{{{"Content-Type", {R"(multipart/alternative; boundary="foo-bar")"}}},
           crlf("--foo-bar\n\nBody\n--foo-bar\n\nBody2\n--foo-bar--")}}},

        // A nested boundary cannot be the outer separator followed by double
        // dash.
        {"nested separator prefix is outer separator followed by double dash",
         crlf("--foo\nContent-Type: multipart/alternative; boundary=\"foo--\"\n\n--foo--\n\nBody\n\n--foo--"), "foo",
         {{{{"Content-Type", {R"(multipart/alternative; boundary="foo--")"}}}, ""}}},

        roundTripParseTest(),
    };
}

std::pair<std::vector<headerBody>, error> partsFromReader(multipart::Reader& r) {
    std::vector<headerBody> got;
    for (;;) {
        auto [p, err] = r.NextPart();
        if (err == io::EOF_) return {got, nullptr};
        if (err) return {{}, fmt::Errorf("NextPart: {}", err)};
        auto [pbody, rerr] = io::ReadAll(*p);
        if (rerr) return {{}, fmt::Errorf("error reading part: {}", rerr)};
        got.push_back({p->Header, str(pbody)});
    }
}

TEST(GoMultipart, Parse) {
    for (const auto& tt : parseTests()) {
        auto r = multipart::NewReader(strings::NewReader(string(tt.in)), tt.sep);
        auto [got, err] = partsFromReader(*r);
        if (err) {
            ADD_FAILURE() << "in test " << strconv::Quote(tt.name) << ", " << err;
            continue;
        }
        EXPECT_EQ(got, tt.want) << "test " << strconv::Quote(tt.name);
    }
}

TEST(GoMultipart, ParseAllSizes) {
    constexpr std::size_t maxSize = 5 << 10;
    std::string body = repeat("a", maxSize);
    for (std::size_t size = 0; size < maxSize; size++) {
        auto buf = make_ptr<bytes::Buffer>();
        auto w = multipart::NewWriter(buf);
        auto [part, _] = w->CreateFormField("f");
        io::WriteString(*part, std::string_view(body).substr(0, size));
        std::tie(part, _) = w->CreateFormField("key");
        io::WriteString(*part, "val");
        w->Close();
        auto r = multipart::NewReader(buf, w->Boundary());
        auto [got, err] = partsFromReader(*r);
        if (err) {
            ADD_FAILURE() << "For size " << size << ": " << err;
            continue;
        }
        if (got.size() != 2) {
            ADD_FAILURE() << "For size " << size << ", num parts = " << got.size() << "; want 2";
            continue;
        }
        if (got[0].body != body.substr(0, size)) {
            ADD_FAILURE() << "For size " << size << ", got unexpected len " << got[0].body.size();
        }
    }
}

TEST(GoMultipart, NoBoundary) {
    auto mr = multipart::NewReader(strings::NewReader(""), "");
    auto [_, err] = mr->NextPart();
    EXPECT_EQ(err.Error(), "multipart: boundary is empty");
}

// ---------------------------------------------------------------------------
// formdata_test.go

const std::string fileaContents = "This is a test file.";
const std::string filebContents = "Another test file.";
const std::string textaValue = "foo";
const std::string textbValue = "bar";
const std::string boundary = "MyBoundary";

const std::string messageWithFileWithoutName = R"(
--MyBoundary
Content-Disposition: form-data; name="hiddenfile"; filename=""
Content-Type: text/plain

)" + filebContents + R"(
--MyBoundary--
)";

const std::string messageWithFileName = R"(
--MyBoundary
Content-Disposition: form-data; name="filea"; filename="filea.txt"
Content-Type: text/plain

)" + fileaContents + R"(
--MyBoundary--
)";

const std::string messageWithTextContentType = R"(
--MyBoundary
Content-Disposition: form-data; name="texta"
Content-Type: text/plain

)" + textaValue + R"(
--MyBoundary
)";

const std::string message = R"(
--MyBoundary
Content-Disposition: form-data; name="filea"; filename="filea.txt"
Content-Type: text/plain

)" + fileaContents + R"(
--MyBoundary
Content-Disposition: form-data; name="fileb"; filename="fileb.txt"
Content-Type: text/plain

)" + filebContents + R"(
--MyBoundary
Content-Disposition: form-data; name="texta"

)" + textaValue + R"(
--MyBoundary
Content-Disposition: form-data; name="textb"

)" + textbValue + R"(
--MyBoundary--
)";

ptr<multipart::File> testFile(const ptr<multipart::FileHeader>& fh, std::string efn, std::string econtent) {
    EXPECT_EQ(fh->Filename, efn);
    EXPECT_EQ(fh->Size, int64(econtent.size()));
    auto [f, err] = fh->Open();
    if (err) {
        ADD_FAILURE() << "opening file: " << err;
        return nullptr;
    }
    strings::Builder b;
    auto [_, cerr] = io::Copy(b, *f);
    EXPECT_FALSE(cerr) << "copying contents: " << cerr;
    EXPECT_EQ(b.String(), econtent);
    return f;
}

TEST(GoMultipart, ReadForm) {
    auto r = multipart::NewReader(strings::NewReader(crlf(message)), boundary);
    auto [f, err] = r->ReadForm(25);
    ASSERT_FALSE(err) << "ReadForm: " << err;
    EXPECT_EQ(f->Value["texta"][0], textaValue);
    EXPECT_EQ(f->Value["textb"][0], textbValue);
    // filea is in memory, fileb on disk (Go checks the type of the file).
    if (auto fd = testFile(f->File["filea"][0], "filea.txt", fileaContents)) fd->Close();
    if (auto fd = testFile(f->File["fileb"][0], "fileb.txt", filebContents)) fd->Close();
    EXPECT_FALSE(f->RemoveAll());
}

TEST(GoMultipart, ReadFormWithNamelessFile) {
    auto r = multipart::NewReader(strings::NewReader(crlf(messageWithFileWithoutName)), boundary);
    auto [f, err] = r->ReadForm(25);
    ASSERT_FALSE(err) << "ReadForm: " << err;
    EXPECT_EQ(f->Value["hiddenfile"][0], filebContents);
    f->RemoveAll();
}

// Issue 58384: Handle ReadForm(math.MaxInt64)
TEST(GoMultipart, ReadFormWitFileNameMaxMemoryOverflow) {
    auto r = multipart::NewReader(strings::NewReader(crlf(messageWithFileName)), boundary);
    auto [f, err] = r->ReadForm(std::numeric_limits<int64>::max());
    ASSERT_FALSE(err) << "ReadForm(MaxInt64): " << err;
    if (auto fd = testFile(f->File["filea"][0], "filea.txt", fileaContents)) fd->Close();
    f->RemoveAll();
}

// Issue 40430: Handle ReadForm(math.MaxInt64)
TEST(GoMultipart, ReadFormMaxMemoryOverflow) {
    auto r = multipart::NewReader(strings::NewReader(crlf(messageWithTextContentType)), boundary);
    auto [f, err] = r->ReadForm(std::numeric_limits<int64>::max());
    ASSERT_FALSE(err) << "ReadForm(MaxInt64): " << err;
    ASSERT_TRUE(f) << "ReadForm(MaxInt64): missing form";
    EXPECT_EQ(f->Value["texta"][0], textaValue);
    f->RemoveAll();
}

TEST(GoMultipart, ReadFormWithTextContentType) {
    // From https://github.com/golang/go/issues/24041
    auto r = multipart::NewReader(strings::NewReader(crlf(messageWithTextContentType)), boundary);
    auto [f, err] = r->ReadForm(25);
    ASSERT_FALSE(err) << "ReadForm: " << err;
    EXPECT_EQ(f->Value["texta"][0], textaValue);
    f->RemoveAll();
}

// A Reader that fails the test if it's read after a failing Read.
struct failOnReadAfterErrorReader final : io::Reader {
    ptr<io::Reader> r;
    error sawErr;
    explicit failOnReadAfterErrorReader(ptr<io::Reader> r_) : r(std::move(r_)) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (sawErr) ADD_FAILURE() << "unexpected Read on Reader after previous read saw error " << sawErr;
        auto [n, err] = r->Read(p);
        sawErr = err;
        return {n, err};
    }
};

TEST(GoMultipart, ReadForm_NoReadAfterEOF) {
    int64 maxMemory = int64(32) << 20;
    std::string body = R"(
-----------------------------8d345eef0d38dc9
Content-Disposition: form-data; name="version"

171
-----------------------------8d345eef0d38dc9--)";
    auto mr = multipart::NewReader(make_ptr<failOnReadAfterErrorReader>(strings::NewReader(body)),
                                   "---------------------------8d345eef0d38dc9");
    auto [f, err] = mr->ReadForm(maxMemory);
    ASSERT_FALSE(err) << err;
}

// TestReadForm_NonFileMaxMemory asserts that the ReadForm maxMemory limit is
// applied while processing non-file form data as well as file form data.
TEST(GoMultipart, ReadForm_NonFileMaxMemory) {
    std::string largeTextValue(10 << 20, '1');
    std::string testBody = crlf("--MyBoundary\nContent-Disposition: form-data; name=\"largetext\"\n\n") + largeTextValue +
                           crlf("\n--MyBoundary--\n");
    // Try parsing the form with increasing maxMemory values. Changes in how
    // we account for non-file form data may cause the exact point where we
    // change from rejecting the form as too large to accepting it to vary,
    // but we should see both successes and failures.
    constexpr int64 failWhenMaxMemoryLessThan = 128;
    for (int64 maxMemory = 0; maxMemory < failWhenMaxMemoryLessThan * 2; maxMemory += 16) {
        auto r = multipart::NewReader(strings::NewReader(string(testBody)), boundary);
        auto [f, err] = r->ReadForm(maxMemory);
        if (err) continue;
        EXPECT_TRUE(f->Value["largetext"][0] == largeTextValue) << "largetext mismatch";
        f->RemoveAll();
        EXPECT_GE(maxMemory, failWhenMaxMemoryLessThan) << "ReadForm: no error, expect to hit memory limit";
        return;
    }
    ADD_FAILURE() << "ReadForm(x) failed for x < 1024, expect success";
}

// TestReadForm_MetadataTooLarge verifies that we account for the size of
// field names, MIME headers, and map entry overhead while limiting the
// memory consumption of parsed forms.
TEST(GoMultipart, ReadForm_MetadataTooLarge) {
    struct test {
        std::string name;
        std::function<void(multipart::Writer&)> f;
    };
    std::vector<test> tests = {
        {"large name",
         [](multipart::Writer& fw) {
             auto [w, _] = fw.CreateFormField(std::string(10 << 20, 'a'));
             io::WriteString(*w, "value");
         }},
        {"large MIME header",
         [](multipart::Writer& fw) {
             MIMEHeader h;
             h.Set("Content-Disposition", R"(form-data; name="a")");
             h.Set("X-Foo", string(std::string(10 << 20, 'a')));
             auto [w, _] = fw.CreatePart(h);
             io::WriteString(*w, "value");
         }},
        {"many parts",
         [](multipart::Writer& fw) {
             for (int i = 0; i < 110000; i++) {
                 auto [w, _] = fw.CreateFormField("f");
                 io::WriteString(*w, "v");
             }
         }},
    };
    for (const auto& test : tests) {
        SCOPED_TRACE(test.name);
        auto buf = make_ptr<bytes::Buffer>();
        auto fw = multipart::NewWriter(buf);
        test.f(*fw);
        ASSERT_FALSE(fw->Close());
        auto fr = multipart::NewReader(buf, fw->Boundary());
        auto [_, err] = fr->ReadForm(0);
        EXPECT_EQ(err, multipart::ErrMessageTooLarge) << err;
    }
}

// TestReadForm_ManyFiles_Combined tests that a multipart form containing
// many files only results in a single on-disk file.
TEST(GoMultipart, ReadForm_ManyFiles_Combined) {
    auto buf = make_ptr<bytes::Buffer>();
    auto fw = multipart::NewWriter(buf);
    constexpr int numFiles = 10;
    for (int i = 0; i < numFiles; i++) {
        auto name = std::to_string(i);
        auto [w, err] = fw->CreateFormFile(name, name);
        ASSERT_FALSE(err);
        io::WriteString(*w, name);
    }
    ASSERT_FALSE(fw->Close());
    auto fr = multipart::NewReader(buf, fw->Boundary());
    auto [dirName, derr] = os::MkdirTemp("", "multipart");
    ASSERT_FALSE(derr);
    fr->tempDir = dirName;
    auto [form, err] = fr->ReadForm(0);
    ASSERT_FALSE(err) << err;
    for (int i = 0; i < numFiles; i++) {
        auto name = std::to_string(i);
        ASSERT_EQ(form->File[name].size(), 1u) << name;
        auto [file, oerr] = form->File[name][0]->Open();
        ASSERT_FALSE(oerr) << oerr;
        auto [got, rerr] = io::ReadAll(*file);
        file->Close();
        ASSERT_EQ(str(got), name);
        ASSERT_FALSE(rerr);
    }
    auto [names, rerr] = os::ReadDir(dirName);
    ASSERT_FALSE(rerr);
    ASSERT_EQ(names.size(), 1u) << "temp dir contains " << names.size() << " files; want 1";
    ASSERT_FALSE(form->RemoveAll());
    std::tie(names, rerr) = os::ReadDir(dirName);
    ASSERT_FALSE(rerr);
    ASSERT_EQ(names.size(), 0u);
    os::RemoveAll(dirName);
}

TEST(GoMultipart, ReadFormLimits) {
    struct test {
        int values, files, extraKeysPerFile;
        error wantErr;
    };
    std::vector<test> tests = {
        {1000, 0, 0, nullptr},
        {1001, 0, 0, multipart::ErrMessageTooLarge},
        {500, 500, 0, nullptr},
        {501, 500, 0, multipart::ErrMessageTooLarge},
        {0, 1000, 0, nullptr},
        {0, 1001, 0, multipart::ErrMessageTooLarge},
        {0, 1, 9998, nullptr},  // plus Content-Disposition and Content-Type
        {0, 1, 10000, multipart::ErrMessageTooLarge},
    };
    for (const auto& test : tests) {
        SCOPED_TRACE(fmt::Sprint("values={}/files={}/extraKeysPerFile={}", test.values, test.files, test.extraKeysPerFile));
        auto buf = make_ptr<bytes::Buffer>();
        auto fw = multipart::NewWriter(buf);
        for (int i = 0; i < test.values; i++) {
            auto [w, _] = fw->CreateFormField(fmt::Sprint("field{}", i));
            fmt::Print(*w, "value {}", i);
        }
        for (int i = 0; i < test.files; i++) {
            MIMEHeader h;
            h.Set("Content-Disposition", fmt::Sprint(R"(form-data; name="file{}"; filename="file{}")", i, i));
            h.Set("Content-Type", "application/octet-stream");
            for (int j = 0; j < test.extraKeysPerFile; j++) h.Set(fmt::Sprint("k{}", j), "v");
            auto [w, _] = fw->CreatePart(h);
            fmt::Print(*w, "value {}", i);
        }
        ASSERT_FALSE(fw->Close());
        auto fr = multipart::NewReader(bytes::NewReader(buf->Bytes()), fw->Boundary());
        auto [form, err] = fr->ReadForm(1 << 10);
        if (!err) form->RemoveAll();
        EXPECT_EQ(err, test.wantErr) << "ReadForm = " << err;
    }
}

struct neverendingReader final : io::Reader {
    byte b;
    explicit neverendingReader(byte c) : b(c) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        for (auto& c : p) c = b;
        return {isize(p.size()), nullptr};
    }
};

TEST(GoMultipart, ReadFormEndlessHeaderLine) {
    for (auto [name, prefix] : std::vector<std::pair<std::string, std::string>>{
             {"name", "X-"}, {"value", "X-Header: "}, {"continuation", "X-Header: foo\r\n  "}}) {
        SCOPED_TRACE(name);
        std::string s = "--boundary\r\nContent-Disposition: form-data; name=\"a\"\r\nContent-Type: text/plain\r\n" + prefix;
        auto fr = io::MultiReader({strings::NewReader(string(s)), make_ptr<neverendingReader>('X')});
        auto r = multipart::NewReader(fr, "boundary");
        auto [_, err] = r->ReadForm(1 << 20);
        EXPECT_EQ(err, multipart::ErrMessageTooLarge) << "ReadForm(1 << 20): " << err;
    }
}

// ---------------------------------------------------------------------------
// writer_test.go

TEST(GoMultipart, Writer) {
    std::string fileContents = "my file contents";
    auto b = make_ptr<bytes::Buffer>();
    auto w = multipart::NewWriter(b);
    {
        auto [part, err] = w->CreateFormFile("myfile", "my-file.txt");
        ASSERT_FALSE(err) << "CreateFormFile: " << err;
        io::WriteString(*part, fileContents);
        ASSERT_FALSE(w->WriteField("key", "val"));
        io::WriteString(*part, "val");
        ASSERT_FALSE(w->Close());
        auto s = b->String();
        ASSERT_FALSE(s.empty()) << "String: unexpected empty result";
        ASSERT_TRUE(s[0] != '\r' && s[0] != '\n') << "String: unexpected newline";
    }

    auto r = multipart::NewReader(b, w->Boundary());

    auto [part, err] = r->NextPart();
    ASSERT_FALSE(err) << "part 1: " << err;
    EXPECT_EQ(part->FormName(), "myfile");
    auto [slurp, rerr] = io::ReadAll(*part);
    ASSERT_FALSE(rerr) << "part 1: ReadAll: " << rerr;
    EXPECT_EQ(str(slurp), fileContents);

    std::tie(part, err) = r->NextPart();
    ASSERT_FALSE(err) << "part 2: " << err;
    EXPECT_EQ(part->FormName(), "key");
    std::tie(slurp, rerr) = io::ReadAll(*part);
    ASSERT_FALSE(rerr) << "part 2: ReadAll: " << rerr;
    EXPECT_EQ(str(slurp), "val");

    std::tie(part, err) = r->NextPart();
    ASSERT_TRUE(!part && err) << "expected end of parts";
}

TEST(GoMultipart, WriterSetBoundary) {
    std::vector<std::pair<std::string, bool>> tests = {
        {"abc", true},
        {"", false},
        {"ungültig", false},
        {"!", false},
        {std::string(70, 'x'), true},
        {std::string(71, 'x'), false},
        {"bad!ascii!", false},
        {"my-separator", true},
        {"with space", true},
        {"badspace ", false},
        {"(boundary)", true},
    };
    int i = 0;
    for (const auto& [b, ok] : tests) {
        auto buf = make_ptr<strings::Builder>();
        auto w = multipart::NewWriter(buf);
        auto err = w->SetBoundary(b);
        bool got = !err;
        EXPECT_EQ(got, ok) << i << ". boundary " << strconv::Quote(b) << " (" << err << ")";
        if (got && ok) {
            EXPECT_EQ(w->Boundary(), b);
            auto ct = w->FormDataContentType();
            auto [mt, params, perr] = mime::ParseMediaType(ct);
            EXPECT_FALSE(perr) << "could not parse Content-Type " << ct << ": " << perr;
            EXPECT_EQ(mt, "multipart/form-data");
            EXPECT_EQ(params["boundary"], b);
            w->Close();
            std::string wantSub = "\r\n--" + b + "--\r\n";
            EXPECT_NE(buf->String().view().find(wantSub), std::string_view::npos) << "expected " << wantSub;
        }
        i++;
    }
}

TEST(GoMultipart, WriterBoundaryGoroutines) {
    // Verify there's no data race accessing any lazy boundary if it's used
    // by different goroutines.
    struct discard final : io::Writer {
        std::pair<isize, error> Write(std::span<const byte> p) override { return {isize(p.size()), nullptr}; }
    };
    auto w = multipart::NewWriter(make_ptr<discard>());
    auto done = make_chan<int>();
    go([w, done] {
        w->CreateFormField("foo");
        done.send(1);
    });
    w->Boundary();
    done.recv();
}

TEST(GoMultipart, SortedHeader) {
    auto buf = make_ptr<strings::Builder>();
    auto w = multipart::NewWriter(buf);
    ASSERT_FALSE(w->SetBoundary("MIMEBOUNDARY"));
    MIMEHeader header = {{"A", {"2"}}, {"B", {"5", "7", "6"}}, {"C", {"4"}}, {"M", {"3"}}, {"Z", {"1"}}};
    auto [part, err] = w->CreatePart(header);
    ASSERT_FALSE(err) << "Unable to create part: " << err;
    io::WriteString(*part, "foo");
    w->Close();
    EXPECT_EQ(buf->String(), "--MIMEBOUNDARY\r\nA: 2\r\nB: 5\r\nB: 7\r\nB: 6\r\nC: 4\r\nM: 3\r\nZ: 1\r\n\r\nfoo\r\n--MIMEBOUNDARY--\r\n");
}

TEST(GoMultipart, FileContentDisposition) {
    struct test {
        std::string fieldname, filename, want;
    };
    std::vector<test> tests = {
        {"somefield", "somefile.txt", R"(form-data; name="somefield"; filename="somefile.txt")"},
        {R"(field"withquotes")", "somefile.txt", R"(form-data; name="field\"withquotes\""; filename="somefile.txt")"},
        {R"(somefield)", R"(somefile"withquotes".txt)", R"(form-data; name="somefield"; filename="somefile\"withquotes\".txt")"},
        {R"(somefield\withbackslash)", "somefile.txt", R"(form-data; name="somefield\\withbackslash"; filename="somefile.txt")"},
        {"somefield", R"(somefile\withbackslash.txt)", R"(form-data; name="somefield"; filename="somefile\\withbackslash.txt")"},
        {"a\rb\nc", "e\rf\ng", R"(form-data; name="a%0Db%0Ac"; filename="e%0Df%0Ag")"},
    };
    int i = 0;
    for (const auto& tt : tests) {
        EXPECT_EQ(multipart::FileContentDisposition(tt.fieldname, tt.filename), tt.want) << i;
        i++;
    }
}

}  // namespace
