// Ported from Go 1.27.3's bytes/buffer_test.go and reader_test.go, with
// Go's test names. Left out: the benchmarks, the allocation counts, the
// concurrency tests for the race detector, TestNewBufferShallow, TestNil
// (a nil *Buffer), TestMixedReadsAndWrites (random) and
// TestReadFromPanicReader (go++ can't recover from a panic).
#include <gopp/bytes.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using namespace gopp;
using namespace std::literals;

namespace {

constexpr std::size_t N = 10000;  // make this bigger for a larger (and slower) test

// test data; testString is the same as testBytes.
const slice<byte> testBytes = [] {
    auto b = slice<byte>::make(N);
    for (std::size_t i = 0; i < N; i++) b[i] = byte('a' + i % 26);
    return b;
}();
const std::string testString(asStringView(testBytes));

std::string str(std::span<const byte> s) { return std::string(asStringView(s)); }

struct negativeReader : io::Reader {
    std::pair<isize, error> Read(std::span<byte>) override { return {-1, nullptr}; }
};

// Verify that contents of buf match the string s.
void check(const std::string& testname, bytes::Buffer& buf, std::string_view s) {
    auto bs = buf.Bytes();
    auto st = buf.String();
    EXPECT_EQ(std::size_t(buf.Len()), bs.len()) << testname << ": buf.Len() != len(buf.Bytes())";
    EXPECT_EQ(std::size_t(buf.Len()), st.len()) << testname << ": buf.Len() != len(buf.String())";
    EXPECT_EQ(std::size_t(buf.Len()), s.size()) << testname << ": buf.Len() != len(s)";
    EXPECT_EQ(asStringView(bs), s) << testname << ": string(buf.Bytes()) != s";
}

// Fill buf through n writes of string fus. The initial contents of buf
// corresponds to the string s; the result is the final contents of buf
// returned as a string.
std::string fillString(const std::string& testname, bytes::Buffer& buf, std::string s, int n, std::string_view fus) {
    check(testname + " (fill 1)", buf, s);
    for (; n > 0; n--) {
        auto [m, err] = buf.WriteString(fus);
        EXPECT_EQ(std::size_t(m), fus.size()) << testname << " (fill 2)";
        EXPECT_FALSE(err) << testname << " (fill 3): err should always be nil";
        s += fus;
        check(testname + " (fill 4)", buf, s);
    }
    return s;
}

// Fill buf through n writes of byte slice fub. The initial contents of
// buf corresponds to the string s; the result is the final contents of buf
// returned as a string.
std::string fillBytes(const std::string& testname, bytes::Buffer& buf, std::string s, int n, std::span<const byte> fub) {
    check(testname + " (fill 1)", buf, s);
    for (; n > 0; n--) {
        auto [m, err] = buf.Write(fub);
        EXPECT_EQ(std::size_t(m), fub.size()) << testname << " (fill 2)";
        EXPECT_FALSE(err) << testname << " (fill 3): err should always be nil";
        s += asStringView(fub);
        check(testname + " (fill 4)", buf, s);
    }
    return s;
}

TEST(GoBytesBuffer, NewBuffer) {
    auto buf = bytes::NewBuffer(testBytes);
    check("NewBuffer", *buf, testString);
}

TEST(GoBytesBuffer, NewBufferString) {
    auto buf = bytes::NewBufferString(testString);
    check("NewBufferString", *buf, testString);
}

// Empty buf through repeated reads into fub. The initial contents of buf
// corresponds to the string s.
void empty(const std::string& testname, bytes::Buffer& buf, std::string s, slice<byte> fub) {
    check(testname + " (empty 1)", buf, s);
    for (;;) {
        auto [n, err] = buf.Read(fub);
        if (n == 0) break;
        EXPECT_FALSE(err) << testname << " (empty 2): err should always be nil";
        s = s.substr(std::size_t(n));
        check(testname + " (empty 3)", buf, s);
    }
    check(testname + " (empty 4)", buf, "");
}

TEST(GoBytesBuffer, BasicOperations) {
    bytes::Buffer buf;
    for (int i = 0; i < 5; i++) {
        check("TestBasicOperations (1)", buf, "");
        buf.Reset();
        check("TestBasicOperations (2)", buf, "");
        buf.Truncate(0);
        check("TestBasicOperations (3)", buf, "");
        auto [n, err] = buf.Write(testBytes.Sub(0, 1));
        EXPECT_TRUE(!err && n == 1) << "Write";
        check("TestBasicOperations (4)", buf, "a");
        buf.WriteByte(byte(testString[1]));
        check("TestBasicOperations (5)", buf, "ab");
        std::tie(n, err) = buf.Write(testBytes.Sub(2, 26));
        EXPECT_TRUE(!err && n == 24) << "Write";
        check("TestBasicOperations (6)", buf, testString.substr(0, 26));
        buf.Truncate(26);
        check("TestBasicOperations (7)", buf, testString.substr(0, 26));
        buf.Truncate(20);
        check("TestBasicOperations (8)", buf, testString.substr(0, 20));
        empty("TestBasicOperations (9)", buf, testString.substr(0, 20), slice<byte>::make(5));
        empty("TestBasicOperations (10)", buf, "", slice<byte>::make(100));
        buf.WriteByte(byte(testString[1]));
        auto [c, cerr] = buf.ReadByte();
        EXPECT_TRUE(!cerr && c == byte(testString[1])) << "ReadByte";
        std::tie(c, cerr) = buf.ReadByte();
        EXPECT_EQ(cerr, io::EOF_) << "ReadByte";
    }
}

TEST(GoBytesBuffer, LargeStringWrites) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillString("TestLargeWrites (1)", buf, "", 5, testString);
        empty("TestLargeStringWrites (2)", buf, s, slice<byte>::make(testString.size() / i));
    }
    check("TestLargeStringWrites (3)", buf, "");
}

TEST(GoBytesBuffer, LargeByteWrites) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillBytes("TestLargeWrites (1)", buf, "", 5, testBytes);
        empty("TestLargeByteWrites (2)", buf, s, slice<byte>::make(testString.size() / i));
    }
    check("TestLargeByteWrites (3)", buf, "");
}

TEST(GoBytesBuffer, LargeStringReads) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillString("TestLargeReads (1)", buf, "", 5, std::string_view(testString).substr(0, testString.size() / i));
        empty("TestLargeReads (2)", buf, s, slice<byte>::make(testString.size()));
    }
    check("TestLargeStringReads (3)", buf, "");
}

TEST(GoBytesBuffer, LargeByteReads) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillBytes("TestLargeReads (1)", buf, "", 5, testBytes.Sub(0, testBytes.len() / i));
        empty("TestLargeReads (2)", buf, s, slice<byte>::make(testString.size()));
    }
    check("TestLargeByteReads (3)", buf, "");
}

TEST(GoBytesBuffer, CapWithPreallocatedSlice) {
    auto buf = bytes::NewBuffer(slice<byte>::make(10));
    EXPECT_EQ(buf->Cap(), 10);
}

TEST(GoBytesBuffer, CapWithSliceAndWrittenData) {
    auto buf = bytes::NewBuffer(slice<byte>::make(0, 10));
    buf->Write(as_bytes("test"));
    EXPECT_EQ(buf->Cap(), 10);
}

TEST(GoBytesBuffer, ReadFrom) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillBytes("TestReadFrom (1)", buf, "", 5, testBytes.Sub(0, testBytes.len() / i));
        bytes::Buffer b;
        b.ReadFrom(buf);
        empty("TestReadFrom (2)", b, s, slice<byte>::make(testString.size()));
    }
}

TEST(GoBytesBuffer, ReadFromNegativeReader) {
    EXPECT_DEATH(
        {
            bytes::Buffer b;
            negativeReader r;
            b.ReadFrom(r);
        },
        "bytes.Buffer: reader returned negative count from Read");
}

TEST(GoBytesBuffer, WriteTo) {
    bytes::Buffer buf;
    for (std::size_t i = 3; i < 30; i += 3) {
        auto s = fillBytes("TestWriteTo (1)", buf, "", 5, testBytes.Sub(0, testBytes.len() / i));
        bytes::Buffer b;
        buf.WriteTo(b);
        empty("TestWriteTo (2)", b, s, slice<byte>::make(testString.size()));
    }
}

TEST(GoBytesBuffer, WriteAppend) {
    bytes::Buffer got;
    std::string want;
    for (int i = 0; i < 1000; i++) {
        auto b = got.AvailableBuffer();
        b = appendSpan(b, as_bytes(std::to_string(i)));
        want += std::to_string(i);
        got.Write(b);
    }
    EXPECT_EQ(str(got.Bytes()), want);
}

TEST(GoBytesBuffer, RuneIO) {
    constexpr rune NRune = 1000;
    // Built a test slice while we write the data
    auto b = slice<byte>::make(std::size_t(utf8::UTFMax * NRune));
    bytes::Buffer buf;
    std::size_t n = 0;
    for (rune r = 0; r < NRune; r++) {
        int size = utf8::EncodeRune(b.span().subspan(n), r);
        auto [nbytes, err] = buf.WriteRune(r);
        ASSERT_FALSE(err) << "WriteRune(" << r << ") error";
        ASSERT_EQ(nbytes, size) << "WriteRune(" << r << ")";
        n += std::size_t(size);
    }
    b = b.Sub(0, n);

    // Check the resulting bytes
    ASSERT_TRUE(bytes::Equal(buf.Bytes(), b)) << "incorrect result from WriteRune";

    byte p[utf8::UTFMax];
    // Read it back with ReadRune
    for (rune r = 0; r < NRune; r++) {
        int size = utf8::EncodeRune(p, r);
        auto [nr, nbytes, err] = buf.ReadRune();
        ASSERT_TRUE(nr == r && nbytes == size && !err) << "ReadRune(" << r << ")";
    }

    // Check that UnreadRune works
    buf.Reset();

    // check at EOF
    ASSERT_TRUE(buf.UnreadRune()) << "UnreadRune at EOF: got no error";
    ASSERT_TRUE(std::get<2>(buf.ReadRune())) << "ReadRune at EOF: got no error";
    ASSERT_TRUE(buf.UnreadRune()) << "UnreadRune after ReadRune at EOF: got no error";

    // check not at EOF
    buf.Write(b);
    for (rune r = 0; r < NRune; r++) {
        auto [r1, size, _] = buf.ReadRune();
        ASSERT_FALSE(buf.UnreadRune()) << "UnreadRune(" << r << ") got error";
        auto [r2, nbytes, err] = buf.ReadRune();
        ASSERT_TRUE(r1 == r2 && r1 == r && nbytes == size && !err) << "ReadRune(" << r << ") after UnreadRune";
    }
}

TEST(GoBytesBuffer, WriteInvalidRune) {
    // Invalid runes, including negative ones, should be written as
    // utf8.RuneError.
    for (rune r : {rune(-1), utf8::MaxRune + 1}) {
        bytes::Buffer buf;
        buf.WriteRune(r);
        check("TestWriteInvalidRune (" + std::to_string(r) + ")", buf, "�");
    }
}

TEST(GoBytesBuffer, Next) {
    const slice<byte> b = {0, 1, 2, 3, 4};
    auto tmp = slice<byte>::make(5);
    for (std::size_t i = 0; i <= 5; i++) {
        for (std::size_t j = i; j <= 5; j++) {
            for (isize k = 0; k <= 6; k++) {
                // 0 <= i <= j <= 5; 0 <= k <= 6
                // Check that if we start with a buffer of length j at
                // offset i and ask for Next(k), we get the right bytes.
                auto buf = bytes::NewBuffer(b.Sub(0, j));
                auto [n, _] = buf->Read(tmp.Sub(0, i));
                ASSERT_EQ(std::size_t(n), i) << "Read " << i;
                auto bb = buf->Next(k);
                std::size_t want = std::min(std::size_t(k), j - i);
                ASSERT_EQ(bb.len(), want) << "in " << i << "," << j << ": len(Next(" << k << "))";
                for (std::size_t l = 0; l < bb.len(); l++) ASSERT_EQ(bb[l], byte(l + i)) << "in " << i << "," << j << ": Next(" << k << ")[" << l << "]";
            }
        }
    }
}

struct readBytesTest {
    std::string buffer;
    byte delim;
    std::vector<std::string> expected;
    error err;
};

const std::vector<readBytesTest>& readBytesTests() {
    static const std::vector<readBytesTest> tests = {
        {"", 0, {""}, io::EOF_},
        {"a\x00"s, 0, {"a\x00"s}, nullptr},
        {"abbbaaaba", 'b', {"ab", "b", "b", "aaab"}, nullptr},
        {"hello\x01world", 1, {"hello\x01"}, nullptr},
        {"foo\nbar", 0, {"foo\nbar"}, io::EOF_},
        {"alpha\nbeta\ngamma\n", '\n', {"alpha\n", "beta\n", "gamma\n"}, nullptr},
        {"alpha\nbeta\ngamma", '\n', {"alpha\n", "beta\n", "gamma"}, io::EOF_},
    };
    return tests;
}

TEST(GoBytesBuffer, ReadBytes) {
    for (const auto& test : readBytesTests()) {
        auto buf = bytes::NewBufferString(test.buffer);
        error err;
        for (const auto& expected : test.expected) {
            slice<byte> bs;
            std::tie(bs, err) = buf->ReadBytes(test.delim);
            EXPECT_EQ(str(bs), expected);
            if (err) break;
        }
        EXPECT_EQ(err, test.err);
    }
}

TEST(GoBytesBuffer, ReadString) {
    for (const auto& test : readBytesTests()) {
        auto buf = bytes::NewBufferString(test.buffer);
        error err;
        for (const auto& expected : test.expected) {
            string s;
            std::tie(s, err) = buf->ReadString(test.delim);
            EXPECT_EQ(s, expected);
            if (err) break;
        }
        EXPECT_EQ(err, test.err);
    }
}

TEST(GoBytesBuffer, Peek) {
    struct peekTest {
        std::string buffer;
        isize skip;
        isize n;
        std::string expected;
        error err;
    };
    const std::vector<peekTest> peekTests = {
        {"", 0, 0, "", nullptr},
        {"aaa", 0, 3, "aaa", nullptr},
        {"foobar", 0, 2, "fo", nullptr},
        {"a", 0, 2, "a", io::EOF_},
        {"helloworld", 4, 3, "owo", nullptr},
        {"helloworld", 5, 5, "world", nullptr},
        {"helloworld", 5, 6, "world", io::EOF_},
        {"helloworld", 10, 1, "", io::EOF_},
    };
    for (const auto& test : peekTests) {
        auto buf = bytes::NewBufferString(test.buffer);
        buf->Next(test.skip);
        auto [bs, err] = buf->Peek(test.n);
        EXPECT_EQ(str(bs), test.expected);
        EXPECT_EQ(err, test.err);
        EXPECT_EQ(buf->Len(), isize(test.buffer.size()) - test.skip) << "bad length after peek";
    }
}

TEST(GoBytesBuffer, Grow) {
    const byte x[] = {'x'};
    const byte y[] = {'y'};
    auto tmp = slice<byte>::make(72);
    for (isize growLen : {0, 100, 1000, 10000, 100000}) {
        for (isize startLen : {0, 100, 1000, 10000, 100000}) {
            auto xBytes = bytes::Repeat(x, startLen);
            auto buf = bytes::NewBuffer(xBytes);
            // If we read, this affects buf.off, which is good to test.
            auto [readBytes, _] = buf->Read(tmp);
            auto yBytes = bytes::Repeat(y, growLen);
            buf->Grow(growLen);
            buf->Write(yBytes);
            // Check that buffer has correct data.
            auto got = buf->Bytes();
            EXPECT_TRUE(bytes::Equal(got.Sub(0, std::size_t(startLen - readBytes)), xBytes.Sub(std::size_t(readBytes))))
                << "bad initial data at " << startLen << " " << growLen;
            EXPECT_TRUE(bytes::Equal(got.Sub(std::size_t(startLen - readBytes), std::size_t(startLen - readBytes + growLen)), yBytes))
                << "bad written data at " << startLen << " " << growLen;
        }
    }
}

TEST(GoBytesBuffer, GrowOverflow) {
    EXPECT_DEATH(
        {
            auto buf = bytes::NewBuffer(slice<byte>::make(1));
            buf->Grow(std::numeric_limits<isize>::max());
        },
        "bytes.Buffer: too large");
}

// Was a bug: used to give EOF reading empty slice at EOF.
TEST(GoBytesBuffer, ReadEmptyAtEOF) {
    bytes::Buffer b;
    auto s = slice<byte>::make(0);
    auto [n, err] = b.Read(s);
    EXPECT_FALSE(err) << "read error";
    EXPECT_EQ(n, 0) << "wrong count";
}

TEST(GoBytesBuffer, UnreadByte) {
    bytes::Buffer b;

    // check at EOF
    ASSERT_TRUE(b.UnreadByte()) << "UnreadByte at EOF: got no error";
    ASSERT_TRUE(b.ReadByte().second) << "ReadByte at EOF: got no error";
    ASSERT_TRUE(b.UnreadByte()) << "UnreadByte after ReadByte at EOF: got no error";

    // check not at EOF
    b.WriteString("abcdefghijklmnopqrstuvwxyz");

    // after unsuccessful read
    auto [n, err] = b.Read({});
    ASSERT_TRUE(n == 0 && !err) << "Read(nil)";
    ASSERT_TRUE(b.UnreadByte()) << "UnreadByte after Read(nil): got no error";

    // after successful read
    ASSERT_FALSE(b.ReadBytes('m').second) << "ReadBytes";
    ASSERT_FALSE(b.UnreadByte()) << "UnreadByte";
    auto [c, cerr] = b.ReadByte();
    ASSERT_FALSE(cerr) << "ReadByte";
    EXPECT_EQ(c, 'm');
}

// Tests that we occasionally compact. Issue 5154.
TEST(GoBytesBuffer, BufferGrowth) {
    bytes::Buffer b;
    auto buf = slice<byte>::make(1024);
    b.Write(buf.Sub(0, 1));
    isize cap0 = 0;
    for (int i = 0; i < 5 << 10; i++) {
        b.Write(buf);
        b.Read(buf);
        if (i == 0) cap0 = b.Cap();
    }
    isize cap1 = b.Cap();
    // (*Buffer).grow allows for 2x capacity slop before sliding, so set
    // our error threshold at 3x.
    EXPECT_LE(cap1, cap0 * 3) << "buffer cap too big (grew from " << cap0 << ")";
}

// reader_test.go

TEST(GoBytesReader, Reader) {
    auto r = bytes::NewReader(toBytes("0123456789"));
    struct T {
        int64 off = 0;
        int seek = 0;
        std::size_t n = 0;
        std::string want = "";
        int64 wantpos = 0;
        error readerr = nullptr;
        std::string seekerr = "";
    };
    const std::vector<T> tests = {
        {.off = 0, .seek = io::SeekStart, .n = 20, .want = "0123456789"},
        {.off = 1, .seek = io::SeekStart, .n = 1, .want = "1"},
        {.off = 1, .seek = io::SeekCurrent, .n = 2, .want = "34", .wantpos = 3},
        {.off = -1, .seek = io::SeekStart, .seekerr = "bytes.Reader.Seek: negative position"},
        {.off = int64(1) << 33, .seek = io::SeekStart, .wantpos = int64(1) << 33, .readerr = io::EOF_},
        {.off = 1, .seek = io::SeekCurrent, .wantpos = (int64(1) << 33) + 1, .readerr = io::EOF_},
        {.seek = io::SeekStart, .n = 5, .want = "01234"},
        {.seek = io::SeekCurrent, .n = 5, .want = "56789"},
        {.off = -1, .seek = io::SeekEnd, .n = 1, .want = "9", .wantpos = 9},
    };
    int i = 0;
    for (const auto& tt : tests) {
        auto [pos, err] = r->Seek(tt.off, tt.seek);
        if (!err && !tt.seekerr.empty()) {
            ADD_FAILURE() << i << ". want seek error " << tt.seekerr;
        } else if (err && err.Error() != tt.seekerr) {
            ADD_FAILURE() << i << ". seek error = " << err.Error() << "; want " << tt.seekerr;
        } else {
            if (tt.wantpos != 0) {
                EXPECT_EQ(tt.wantpos, pos) << i << ". pos";
            }
            auto buf = slice<byte>::make(tt.n);
            auto [n, rerr] = r->Read(buf);
            if (rerr != tt.readerr) {
                ADD_FAILURE() << i << ". read = " << rerr << "; want " << tt.readerr;
            } else {
                EXPECT_EQ(str(buf.Sub(0, std::size_t(n))), tt.want) << i;
            }
        }
        i++;
    }
}

TEST(GoBytesReader, ReadAfterBigSeek) {
    auto r = bytes::NewReader(toBytes("0123456789"));
    ASSERT_FALSE(r->Seek((int64(1) << 31) + 5, io::SeekStart).second);
    auto [n, err] = r->Read(slice<byte>::make(10));
    EXPECT_TRUE(n == 0 && err == io::EOF_) << "Read = " << n << ", " << err << "; want 0, EOF";
}

TEST(GoBytesReader, ReaderAt) {
    auto r = bytes::NewReader(toBytes("0123456789"));
    struct T {
        int64 off;
        std::size_t n;
        std::string want;
        std::string wanterr;
    };
    const std::vector<T> tests = {
        {0, 10, "0123456789", ""},
        {1, 10, "123456789", "EOF"},
        {1, 9, "123456789", ""},
        {11, 10, "", "EOF"},
        {0, 0, "", ""},
        {-1, 0, "", "bytes.Reader.ReadAt: negative offset"},
    };
    int i = 0;
    for (const auto& tt : tests) {
        auto b = slice<byte>::make(tt.n);
        auto [rn, err] = r->ReadAt(b, tt.off);
        EXPECT_EQ(str(b.Sub(0, std::size_t(rn))), tt.want) << i;
        EXPECT_EQ(err ? std::string(err.Error().view()) : "", tt.wanterr) << i;
        i++;
    }
}

TEST(GoBytesReader, ReaderWriteTo) {
    for (std::size_t i = 0; i < 30; i += 3) {
        std::size_t l = i > 0 ? testString.size() / i : 0;
        auto s = testString.substr(0, l);
        auto r = bytes::NewReader(testBytes.Sub(0, l));
        bytes::Buffer b;
        auto [n, err] = r->WriteTo(b);
        EXPECT_EQ(n, int64(s.size()));
        EXPECT_FALSE(err) << "for length " << l;
        EXPECT_EQ(b.String(), s);
        EXPECT_EQ(r->Len(), 0) << "reader contains bytes";
    }
}

TEST(GoBytesReader, ReaderLen) {
    auto r = bytes::NewReader(toBytes("hello world"));
    EXPECT_EQ(r->Len(), 11);
    auto [n, err] = r->Read(slice<byte>::make(10));
    EXPECT_TRUE(!err && n == 10) << "Read failed";
    EXPECT_EQ(r->Len(), 1);
    std::tie(n, err) = r->Read(slice<byte>::make(1));
    EXPECT_TRUE(!err && n == 1) << "Read failed";
    EXPECT_EQ(r->Len(), 0);
}

TEST(GoBytesReader, UnreadRuneError) {
    struct T {
        const char* name;
        std::function<void(bytes::Reader&)> f;
    };
    const std::vector<T> UnreadRuneErrorTests = {
        {"Read", [](bytes::Reader& r) { r.Read(slice<byte>{0}); }},
        {"ReadByte", [](bytes::Reader& r) { r.ReadByte(); }},
        {"UnreadRune", [](bytes::Reader& r) { r.UnreadRune(); }},
        {"Seek", [](bytes::Reader& r) { r.Seek(0, io::SeekCurrent); }},
        {"WriteTo", [](bytes::Reader& r) {
             bytes::Buffer b;
             r.WriteTo(b);
         }},
    };
    for (const auto& tt : UnreadRuneErrorTests) {
        auto reader = bytes::NewReader(toBytes("0123456789"));
        ASSERT_FALSE(std::get<2>(reader->ReadRune()));
        tt.f(*reader);
        EXPECT_TRUE(reader->UnreadRune()) << "Unreading after " << tt.name << ": expected error";
    }
}

TEST(GoBytesReader, ReaderDoubleUnreadRune) {
    auto buf = bytes::NewBuffer(toBytes("groucho"));
    ASSERT_FALSE(std::get<2>(buf->ReadRune()));
    ASSERT_FALSE(buf->UnreadByte());
    ASSERT_TRUE(buf->UnreadByte()) << "UnreadByte: expected error, got nil";
}

// verify that copying from an empty reader always has the same results,
// regardless of the presence of a WriteTo method.
TEST(GoBytesReader, ReaderCopyNothing) {
    struct justReader : io::Reader {
        explicit justReader(io::Reader& r) : r_(r) {}
        std::pair<isize, error> Read(std::span<byte> p) override { return r_.Read(p); }
        io::Reader& r_;
    };
    struct justWriter : io::Writer {
        std::pair<isize, error> Write(std::span<const byte> p) override { return io::Discard().Write(p); }
    };
    justWriter discard;  // hide ReadFrom
    bytes::Reader r1, r2;
    justReader jr(r2);
    auto with = io::Copy(discard, r1);
    auto withOut = io::Copy(discard, jr);
    EXPECT_EQ(with.first, withOut.first);
    EXPECT_EQ(with.second, withOut.second);
}

// tests that Len is affected by reads, but Size is not.
TEST(GoBytesReader, ReaderLenSize) {
    auto r = bytes::NewReader(toBytes("abc"));
    io::CopyN(io::Discard(), *r, 1);
    EXPECT_EQ(r->Len(), 2);
    EXPECT_EQ(r->Size(), 3);
}

TEST(GoBytesReader, ReaderReset) {
    auto r = bytes::NewReader(toBytes("世界"));
    EXPECT_FALSE(std::get<2>(r->ReadRune())) << "ReadRune: unexpected error";
    const std::string want = "abcdef";
    r->Reset(toBytes(want));
    EXPECT_TRUE(r->UnreadRune()) << "UnreadRune: expected error, got nil";
    auto [buf, err] = io::ReadAll(*r);
    EXPECT_FALSE(err) << "ReadAll: unexpected error";
    EXPECT_EQ(str(buf), want);
}

TEST(GoBytesReader, ReaderZero) {
    EXPECT_EQ(bytes::Reader().Len(), 0);
    {
        auto [n, err] = bytes::Reader().Read({});
        EXPECT_TRUE(n == 0 && err == io::EOF_) << "Read";
    }
    {
        auto [n, err] = bytes::Reader().ReadAt({}, 11);
        EXPECT_TRUE(n == 0 && err == io::EOF_) << "ReadAt";
    }
    {
        auto [b, err] = bytes::Reader().ReadByte();
        EXPECT_TRUE(b == 0 && err == io::EOF_) << "ReadByte";
    }
    {
        auto [ch, size, err] = bytes::Reader().ReadRune();
        EXPECT_TRUE(ch == 0 && size == 0 && err == io::EOF_) << "ReadRune";
    }
    {
        auto [offset, err] = bytes::Reader().Seek(11, io::SeekStart);
        EXPECT_TRUE(offset == 11 && !err) << "Seek";
    }
    EXPECT_EQ(bytes::Reader().Size(), 0);
    EXPECT_TRUE(bytes::Reader().UnreadByte()) << "UnreadByte: got nil, want error";
    EXPECT_TRUE(bytes::Reader().UnreadRune()) << "UnreadRune: got nil, want error";
    {
        auto [n, err] = bytes::Reader().WriteTo(io::Discard());
        EXPECT_TRUE(n == 0 && !err) << "WriteTo";
    }
}

}  // namespace
