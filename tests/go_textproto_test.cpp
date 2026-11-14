// Ported from Go 1.27.3's net/textproto/reader_test.go, header_test.go and
// writer_test.go, with Go's test names. Left out: the tests of allocations,
// of the common header table and of upcomingHeaderKeys, which go++ doesn't
// have, and the benchmarks.
#include <gopp/bufio.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/net/textproto.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

using namespace gopp;
using textproto::MIMEHeader;

namespace {

ptr<textproto::Reader> reader(std::string s) {
    return textproto::NewReader(make_ptr<bufio::Reader>(strings::NewReader(string(s))));
}

std::string repeat(std::string_view s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

TEST(GoTextproto, ReadLine) {
    auto r = reader("line1\nline2\n");
    auto [s, err] = r->ReadLine();
    ASSERT_EQ(s, "line1");
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadLine();
    ASSERT_EQ(s, "line2");
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadLine();
    ASSERT_EQ(s, "");
    ASSERT_EQ(err, io::EOF_);
}

TEST(GoTextproto, ReadLineLongLine) {
    auto line = repeat("12345", 10000);
    auto r = reader(line + "\r\n");
    auto [s, err] = r->ReadLine();
    ASSERT_FALSE(err);
    ASSERT_EQ(s.size(), line.size());
    ASSERT_EQ(s, line);
}

TEST(GoTextproto, ReadContinuedLine) {
    auto r = reader("line1\nline\n 2\nline3\n");
    auto [s, err] = r->ReadContinuedLine();
    ASSERT_EQ(s, "line1");
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadContinuedLine();
    ASSERT_EQ(s, "line 2");
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadContinuedLine();
    ASSERT_EQ(s, "line3");
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadContinuedLine();
    ASSERT_EQ(s, "");
    ASSERT_EQ(err, io::EOF_);
}

TEST(GoTextproto, ReadCodeLine) {
    auto r = reader("123 hi\n234 bye\n345 no way\n");
    auto [code, msg, err] = r->ReadCodeLine(0);
    ASSERT_EQ(code, 123);
    ASSERT_EQ(msg, "hi");
    ASSERT_FALSE(err);
    std::tie(code, msg, err) = r->ReadCodeLine(23);
    ASSERT_EQ(code, 234);
    ASSERT_EQ(msg, "bye");
    ASSERT_FALSE(err);
    std::tie(code, msg, err) = r->ReadCodeLine(346);
    ASSERT_EQ(code, 345);
    ASSERT_EQ(msg, "no way");
    ASSERT_TRUE(err);
    auto* e = errors::As<textproto::Error>(err);
    ASSERT_TRUE(e) << "Line 3: wrong error " << err;
    ASSERT_EQ(e->Code, code);
    ASSERT_EQ(e->Msg, msg);
    std::tie(code, msg, err) = r->ReadCodeLine(1);
    ASSERT_EQ(code, 0);
    ASSERT_EQ(msg, "");
    ASSERT_EQ(err, io::EOF_);
}

TEST(GoTextproto, ReadDotLines) {
    auto r = reader("dotlines\r\n.foo\r\n..bar\n...baz\nquux\r\n\r\n.\r\nanother\n");
    auto [s, err] = r->ReadDotLines();
    std::vector<string> want = {"dotlines", "foo", ".bar", "..baz", "quux", ""};
    ASSERT_EQ(s, want);
    ASSERT_FALSE(err);
    std::tie(s, err) = r->ReadDotLines();
    want = {"another"};
    ASSERT_EQ(s, want);
    ASSERT_EQ(err, io::ErrUnexpectedEOF);
}

TEST(GoTextproto, ReadDotBytes) {
    auto r = reader("dotlines\r\n.foo\r\n..bar\n...baz\nquux\r\n\r\n.\r\nanot.her\r\n");
    auto [b, err] = r->ReadDotBytes();
    ASSERT_EQ(std::string(asStringView(b)), "dotlines\nfoo\n.bar\n..baz\nquux\n\n");
    ASSERT_FALSE(err);
    std::tie(b, err) = r->ReadDotBytes();
    ASSERT_EQ(std::string(asStringView(b)), "anot.her\n");
    ASSERT_EQ(err, io::ErrUnexpectedEOF);
}

TEST(GoTextproto, ReadMIMEHeader) {
    auto r = reader("my-key: Value 1  \r\nLong-key: Even \n Longer Value\r\nmy-Key: Value 2\r\n\n");
    auto [m, err] = r->ReadMIMEHeader();
    MIMEHeader want = {{"My-Key", {"Value 1", "Value 2"}}, {"Long-Key", {"Even Longer Value"}}};
    ASSERT_EQ(m, want);
    ASSERT_FALSE(err);
}

TEST(GoTextproto, ReadMIMEHeaderSingle) {
    auto r = reader("Foo: bar\n\n");
    auto [m, err] = r->ReadMIMEHeader();
    MIMEHeader want = {{"Foo", {"bar"}}};
    ASSERT_EQ(m, want);
    ASSERT_FALSE(err);
}

// TestReaderUpcomingHeaderKeys is left out: go++ doesn't presize headers.

TEST(GoTextproto, ReadMIMEHeaderNoKey) {
    auto r = reader(": bar\ntest-1: 1\n\n");
    auto [m, err] = r->ReadMIMEHeader();
    ASSERT_EQ(m, MIMEHeader{});
    ASSERT_TRUE(err);
}

TEST(GoTextproto, LargeReadMIMEHeader) {
    std::string sdata(16 * 1024, 'x');
    auto r = reader("Cookie: " + sdata + "\r\n\n");
    auto [m, err] = r->ReadMIMEHeader();
    ASSERT_FALSE(err);
    auto cookie = m.Get("Cookie");
    ASSERT_EQ(cookie.size(), sdata.size());
    ASSERT_EQ(cookie, sdata);
}

// TestReadMIMEHeaderNonCompliant checks that we don't normalize headers
// with spaces before colons, and accept spaces in keys.
TEST(GoTextproto, ReadMIMEHeaderNonCompliant) {
    // These invalid headers will be rejected by net/http according to RFC
    // 7230.
    auto r = reader("Foo: bar\r\nContent-Language: en\r\nSID : 0\r\nAudio Mode : None\r\nPrivilege : 127\r\n\r\n");
    auto [m, err] = r->ReadMIMEHeader();
    MIMEHeader want = {
        {"Foo", {"bar"}}, {"Content-Language", {"en"}}, {"SID ", {"0"}}, {"Audio Mode ", {"None"}}, {"Privilege ", {"127"}},
    };
    ASSERT_EQ(m, want);
    ASSERT_FALSE(err);
}

TEST(GoTextproto, ReadMIMEHeaderMalformed) {
    std::vector<std::string> inputs = {
        "No colon first line\r\nFoo: foo\r\n\r\n",
        " No colon first line with leading space\r\nFoo: foo\r\n\r\n",
        "\tNo colon first line with leading tab\r\nFoo: foo\r\n\r\n",
        " First: line with leading space\r\nFoo: foo\r\n\r\n",
        "\tFirst: line with leading tab\r\nFoo: foo\r\n\r\n",
        "Foo: foo\r\nNo colon second line\r\n\r\n",
        "Foo-\n\tBar: foo\r\n\r\n",
        "Foo-\r\n\tBar: foo\r\n\r\n",
        "Foo\r\n\t: foo\r\n\r\n",
        "Foo-\n\tBar",
        "Foo \tBar: foo\r\n\r\n",
        ": empty key\r\n\r\n",
    };
    for (const auto& input : inputs) {
        auto r = reader(input);
        auto [m, err] = r->ReadMIMEHeader();
        EXPECT_TRUE(err && err != io::EOF_) << "ReadMIMEHeader(" << input << ") = " << err << "; want err";
    }
}

TEST(GoTextproto, ReadMIMEHeaderBytes) {
    for (int i = 0; i <= 0xff; i++) {
        // Go's string(rune(i)) is the UTF-8 encoding of i.
        std::string r8 = i < 0x80 ? std::string(1, char(i)) : std::string{char(0xc0 | (i >> 6)), char(0x80 | (i & 0x3f))};
        std::string s = "Foo" + r8 + "Bar: foo\r\n\r\n";
        auto r = reader(s);
        bool wantErr = true;
        if ((i >= '0' && i <= '9') || (i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z')) wantErr = false;
        if (std::string_view("!#$%&'*+-.^_`|~").find(char(i)) != std::string_view::npos && i != 0) wantErr = false;
        if (i == ':' || i == ' ') wantErr = false;
        auto [m, err] = r->ReadMIMEHeader();
        EXPECT_EQ(bool(err), wantErr) << "ReadMIMEHeader(" << strconv::Quote(s) << ") = " << err;
    }
    for (int i = 0; i <= 0xff; i++) {
        std::string r8 = i < 0x80 ? std::string(1, char(i)) : std::string{char(0xc0 | (i >> 6)), char(0x80 | (i & 0x3f))};
        std::string s = "Foo: foo" + r8 + "bar\r\n\r\n";
        auto r = reader(s);
        bool wantErr = true;
        if ((i >= 0x21 && i <= 0x7e) || i == ' ' || i == '\t' || (i >= 0x80 && i <= 0xff)) wantErr = false;
        auto [m, err] = r->ReadMIMEHeader();
        EXPECT_EQ(bool(err), wantErr) << "ReadMIMEHeader(" << strconv::Quote(s) << ") = " << err;
    }
}

// Test that continued lines are properly trimmed. Issue 11204.
TEST(GoTextproto, ReadMIMEHeaderTrimContinued) {
    // In this header, \n and \r\n terminated lines are mixed on purpose. We
    // expect each line to be trimmed (prefix and suffix) before being
    // concatenated. Keep the spaces as they are.
    auto r = reader("a:\n 0 \r\nb:1 \t\r\nc: 2\r\n 3\t\n  \t 4  \r\n\n");
    auto [m, err] = r->ReadMIMEHeader();
    ASSERT_FALSE(err);
    MIMEHeader want = {{"A", {"0"}}, {"B", {"1"}}, {"C", {"2 3 4"}}};
    ASSERT_EQ(m, want);
}

struct testReader final : io::Reader {
    std::pair<isize, error> Read(std::span<byte> p) override {
        for (auto& c : p) c = ' ';
        return {isize(p.size()), nullptr};
    }
};

// Test that reading a header doesn't overallocate. Issue 58975.
TEST(GoTextproto, ReadMIMEHeaderShortLimitLongLine) {
    auto r = textproto::NewReader(
        make_ptr<bufio::Reader>(io::MultiReader({strings::NewReader("K:"), make_ptr<testReader>()})));
    auto [_, err] = r->readMIMEHeader(1, 1);
    ASSERT_EQ(err, textproto::errMessageTooLarge) << err;
}

struct readResponseTest {
    std::string in;
    int inCode;
    int wantCode;
    std::string wantMsg;
};

TEST(GoTextproto, RFC959Lines) {
    std::vector<readResponseTest> readResponseTests = {
        {"230-Anonymous access granted, restrictions apply\nRead the file README.txt,\n230  please", 23, 230,
         "Anonymous access granted, restrictions apply\nRead the file README.txt,\n please"},
        {"230 Anonymous access granted, restrictions apply\n", 23, 230, "Anonymous access granted, restrictions apply"},
        {"400-A\n400-B\n400 C", 4, 400, "A\nB\nC"},
        {"400-A\r\n400-B\r\n400 C\r\n", 4, 400, "A\nB\nC"},
    };
    int i = 0;
    for (const auto& tt : readResponseTests) {
        auto r = reader(tt.in + "\nFOLLOWING DATA");
        auto [code, msg, err] = r->ReadResponse(tt.inCode);
        EXPECT_FALSE(err) << "#" << i << ": ReadResponse: " << err;
        EXPECT_EQ(code, tt.wantCode) << "#" << i;
        EXPECT_EQ(msg, tt.wantMsg) << "#" << i;
        i++;
    }
}

// Test that multi-line errors are appropriately and fully read. Issue 10230.
TEST(GoTextproto, ReadMultiLineError) {
    auto r = reader(
        "550-5.1.1 The email account that you tried to reach does not exist. Please try\n"
        "550-5.1.1 double-checking the recipient's email address for typos or\n"
        "550-5.1.1 unnecessary spaces. Learn more at\n"
        "Unexpected but legal text!\n"
        "550 5.1.1 https://support.google.com/mail/answer/6596 h20si25154304pfd.166 - gsmtp\n");
    std::string wantMsg =
        "5.1.1 The email account that you tried to reach does not exist. Please try\n"
        "5.1.1 double-checking the recipient's email address for typos or\n"
        "5.1.1 unnecessary spaces. Learn more at\n"
        "Unexpected but legal text!\n"
        "5.1.1 https://support.google.com/mail/answer/6596 h20si25154304pfd.166 - gsmtp";
    std::string wantError =
        R"(550 "5.1.1 The email account that you tried to reach does not exist. Please try\n5.1.1 double-checking the recipient's email address for typos or\n5.1.1 unnecessary spaces. Learn more at\nUnexpected but legal text!\n5.1.1 https://support.google.com/mail/answer/6596 h20si25154304pfd.166 - gsmtp")";
    auto [code, msg, err] = r->ReadResponse(250);
    EXPECT_TRUE(err) << "ReadResponse: no error, want error";
    EXPECT_EQ(code, 550);
    EXPECT_EQ(msg, wantMsg);
    if (err) {
        EXPECT_EQ(err.Error(), wantError);
    }
}

// Go's TestIssue46363 reads a header from a textproto.Conn over a
// net.Pipe; here, it checks the Conn's reading.
TEST(GoTextproto, Issue46363) {
    auto [r, w] = net::Pipe();
    struct rwc final : io::ReadWriteCloser {
        ptr<net::Conn> c;
        std::pair<isize, error> Read(std::span<byte> p) override { return c->Read(p); }
        std::pair<isize, error> Write(std::span<const byte> p) override { return c->Write(p); }
        error Close() override { return c->Close(); }
    };
    auto rc = make_ptr<rwc>();
    rc->c = r;
    auto done = makeChan<MIMEHeader>(1);
    go([rc, done] { done.send(textproto::NewConn(rc)->ReadMIMEHeader().first); });
    io::WriteString(*w, "A: 1\r\nB: 2\r\nC: 3\r\n\r\n");
    MIMEHeader want = {{"A", {"1"}}, {"B", {"2"}}, {"C", {"3"}}};
    EXPECT_EQ(done.recv(), want);
}

// ---------------------------------------------------------------------------
// header_test.go

TEST(GoTextproto, CanonicalMIMEHeaderKey) {
    std::vector<std::pair<std::string, std::string>> canonicalHeaderKeyTests = {
        {"a-b-c", "A-B-C"},
        {"a-1-c", "A-1-C"},
        {"User-Agent", "User-Agent"},
        {"uSER-aGENT", "User-Agent"},
        {"user-agent", "User-Agent"},
        {"USER-AGENT", "User-Agent"},
        // Other valid tchar bytes in tokens.
        {"foo-bar_baz", "Foo-Bar_baz"},
        {"foo-bar$baz", "Foo-Bar$baz"},
        {"foo-bar~baz", "Foo-Bar~baz"},
        {"foo-bar*baz", "Foo-Bar*baz"},
        // Non-ASCII or anything with spaces or non-token chars is
        // unchanged.
        {"üser-agenT", "üser-agenT"},
        {"a B", "a B"},
        // This caused a panic due to mishandling of a space:
        {"C Ontent-Transfer-Encoding", "C Ontent-Transfer-Encoding"},
        {"foo bar", "foo bar"},
    };
    for (const auto& [in, out] : canonicalHeaderKeyTests) {
        EXPECT_EQ(textproto::CanonicalMIMEHeaderKey(in), out) << in;
    }
}

// Issue #34799 add a Header method to get multiple values [] from the
// header.
TEST(GoTextproto, MIMEHeaderMultipleValues) {
    MIMEHeader testHeader = {{"Set-Cookie", {"cookie 1", "cookie 2"}}};
    EXPECT_EQ(testHeader.Values("set-cookie").size(), 2u);
}

// ---------------------------------------------------------------------------
// writer_test.go

TEST(GoTextproto, PrintfLine) {
    auto buf = make_ptr<strings::Builder>();
    auto w = textproto::NewWriter(make_ptr<bufio::Writer>(buf));
    auto err = w->PrintfLine("foo {}", 123);
    ASSERT_EQ(buf->String(), "foo 123\r\n");
    ASSERT_FALSE(err);
}

TEST(GoTextproto, DotWriter) {
    auto buf = make_ptr<strings::Builder>();
    auto w = textproto::NewWriter(make_ptr<bufio::Writer>(buf));
    auto d = w->DotWriter();
    auto [n, err] = io::WriteString(*d, "abc\n.def\n..ghi\n.jkl\n.");
    ASSERT_EQ(n, 21);
    ASSERT_FALSE(err);
    d->Close();
    ASSERT_EQ(buf->String(), "abc\r\n..def\r\n...ghi\r\n..jkl\r\n..\r\n.\r\n");
}

TEST(GoTextproto, DotWriterCloseEmptyWrite) {
    auto buf = make_ptr<strings::Builder>();
    auto w = textproto::NewWriter(make_ptr<bufio::Writer>(buf));
    auto d = w->DotWriter();
    auto [n, err] = d->Write({});
    ASSERT_EQ(n, 0);
    ASSERT_FALSE(err);
    d->Close();
    ASSERT_EQ(buf->String(), "\r\n.\r\n");
}

TEST(GoTextproto, DotWriterCloseNoWrite) {
    auto buf = make_ptr<strings::Builder>();
    auto w = textproto::NewWriter(make_ptr<bufio::Writer>(buf));
    auto d = w->DotWriter();
    d->Close();
    ASSERT_EQ(buf->String(), "\r\n.\r\n");
}

// A Pipeline puts requests and responses in order.
TEST(GoTextproto, Pipeline) {
    textproto::Pipeline p;
    std::vector<int> order;
    sync::Mutex mu;
    auto done = makeChan<empty>(3);
    uint64 ids[3] = {p.Next(), p.Next(), p.Next()};
    for (int i = 2; i >= 0; i--) {
        go([&, i] {
            p.StartResponse(ids[i]);
            {
                std::lock_guard l(mu);
                order.push_back(i);
            }
            p.EndResponse(ids[i]);
            done.send({});
        });
    }
    for (int i = 0; i < 3; i++) done.recv();
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
}

}  // namespace
