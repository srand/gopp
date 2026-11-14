// Ported from Go 1.27.3's io/pipe_test.go, io/multi_test.go and the
// SectionReader, OffsetWriter, TeeReader and CopyBuffer tests of
// io/io_test.go, with Go's test names. Left out: the tests of allocations
// and call depths.
#include <gopp/bufio.hpp>
#include <gopp/bytes.hpp>
#include <gopp/chan.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

// ---------------------------------------------------------------------------
// Pipe

void checkWrite(io::Writer& w, std::string data, chan<int> c) {
    auto [n, err] = w.Write(as_bytes(data));
    EXPECT_FALSE(err) << "write: " << err;
    EXPECT_EQ(n, isize(data.size())) << "short write";
    c.send(0);
}

TEST(GoIO, Pipe1) {
    auto c = makeChan<int>();
    auto [r, w] = io::Pipe();
    byte buf[64];
    go([w, c] { checkWrite(*w, "hello, world", c); });
    auto [n, err] = r->Read(buf);
    EXPECT_FALSE(err);
    EXPECT_EQ(n, 12);
    EXPECT_EQ(str(std::span(buf, std::size_t(n))), "hello, world");
    c.recv();
    r->Close();
    w->Close();
}

void reader(ptr<io::Reader> r, chan<int> c) {
    byte buf[64];
    for (;;) {
        auto [n, err] = r->Read(buf);
        if (err == io::EOF_) {
            c.send(0);
            break;
        }
        EXPECT_FALSE(err) << "read: " << err;
        c.send(int(n));
    }
}

TEST(GoIO, Pipe2) {
    auto c = makeChan<int>();
    auto [r, w] = io::Pipe();
    go([r, c] { reader(r, c); });
    byte buf[64] = {};
    for (int i = 0; i < 5; i++) {
        auto p = std::span<const byte>(buf, std::size_t(5 + i * 10));
        auto [n, err] = w->Write(p);
        EXPECT_EQ(n, isize(p.size()));
        EXPECT_FALSE(err);
        int nn = c.recv();
        EXPECT_EQ(nn, n);
    }
    w->Close();
    EXPECT_EQ(c.recv(), 0) << "final read";
}

struct pipeReturn {
    isize n;
    error err;
};

TEST(GoIO, Pipe3) {
    // Test a large write that requires multiple reads to satisfy.
    auto c = makeChan<pipeReturn>();
    auto [r, w] = io::Pipe();
    auto wdat = slice<byte>::make(128);
    for (std::size_t i = 0; i < wdat.len(); i++) wdat[i] = byte(i);
    go([w, wdat, c] {
        auto [n, err] = w->Write(wdat);
        w->Close();
        c.send({n, err});
    });
    byte rdat[1024];
    isize tot = 0;
    for (int n = 1; n <= 256; n *= 2) {
        auto [nn, err] = r->Read(std::span(rdat + tot, std::size_t(n)));
        ASSERT_TRUE(!err || err == io::EOF_) << "read: " << err;
        // Only read 128 bytes, so the last read will be truncated.
        isize expect = n;
        if (n == 128) {
            expect = 1;
        } else if (n == 256) {
            expect = 0;
            ASSERT_EQ(err, io::EOF_) << "read at end: " << err;
        }
        ASSERT_EQ(nn, expect) << "read " << n;
        tot += nn;
    }
    auto pr = c.recv();
    ASSERT_EQ(pr.n, 128);
    ASSERT_FALSE(pr.err);
    ASSERT_EQ(tot, 128);
    for (int i = 0; i < 128; i++) ASSERT_EQ(rdat[i], byte(i));
}

// Test read after/before writer close.

struct pipeTest {
    bool async;
    error err;
    bool closeWithError;
};

std::vector<pipeTest> pipeTests() {
    return {
        {true, nullptr, false}, {true, nullptr, true}, {true, io::ErrShortWrite, true},
        {false, nullptr, false}, {false, nullptr, true}, {false, io::ErrShortWrite, true},
    };
}

template <typename C>
void delayClose(C& cl, chan<int> ch, const pipeTest& tt) {
    time::Sleep(1 * time::Millisecond);
    error err = tt.closeWithError ? cl.CloseWithError(tt.err) : cl.Close();
    EXPECT_FALSE(err) << "delayClose: " << err;
    ch.send(0);
}

TEST(GoIO, PipeReadClose) {
    for (const auto& tt : pipeTests()) {
        auto c = makeChan<int>(1);
        auto [r, w] = io::Pipe();
        if (tt.async) {
            go([w, c, tt] { delayClose(*w, c, tt); });
        } else {
            delayClose(*w, c, tt);
        }
        byte buf[64];
        auto [n, err] = r->Read(buf);
        c.recv();
        error want = tt.err ? tt.err : io::EOF_;
        EXPECT_EQ(err, want) << "read from closed pipe: " << err << " want " << want;
        EXPECT_EQ(n, 0) << "read on closed pipe";
        EXPECT_FALSE(r->Close());
    }
}

// Test close on Read side during Read.
TEST(GoIO, PipeReadClose2) {
    auto c = makeChan<int>(1);
    auto [r, w] = io::Pipe();
    go([r, c] { delayClose(*r, c, pipeTest{}); });
    byte buf[64];
    auto [n, err] = r->Read(buf);
    c.recv();
    EXPECT_EQ(n, 0);
    EXPECT_EQ(err, io::ErrClosedPipe);
}

// Test write after/before reader close.
TEST(GoIO, PipeWriteClose) {
    for (const auto& tt : pipeTests()) {
        auto c = makeChan<int>(1);
        auto [r, w] = io::Pipe();
        if (tt.async) {
            go([r, c, tt] { delayClose(*r, c, tt); });
        } else {
            delayClose(*r, c, tt);
        }
        auto [n, err] = io::WriteString(*w, "hello, world");
        c.recv();
        error expect = tt.err ? tt.err : io::ErrClosedPipe;
        EXPECT_EQ(err, expect) << "write on closed pipe: " << err << " want " << expect;
        EXPECT_EQ(n, 0) << "write on closed pipe";
        EXPECT_FALSE(w->Close());
    }
}

// Test close on Write side during Write.
TEST(GoIO, PipeWriteClose2) {
    auto c = makeChan<int>(1);
    auto [r, w] = io::Pipe();
    go([w, c] { delayClose(*w, c, pipeTest{}); });
    byte buf[64] = {};
    auto [n, err] = w->Write(buf);
    c.recv();
    EXPECT_EQ(n, 0);
    EXPECT_EQ(err, io::ErrClosedPipe);
}

TEST(GoIO, WriteEmpty) {
    auto [r, w] = io::Pipe();
    go([w] {
        w->Write({});
        w->Close();
    });
    byte b[2];
    io::ReadFull(*r, b);
    r->Close();
}

TEST(GoIO, WriteAfterWriterClose) {
    auto [r, w] = io::Pipe();
    auto done = makeChan<bool>();
    auto writeErr = std::make_shared<error>();
    go([w, done, writeErr] {
        auto [_, err] = io::WriteString(*w, "hello");
        EXPECT_FALSE(err) << "got error: " << err << "; expected none";
        w->Close();
        *writeErr = io::WriteString(*w, "world").second;
        done.send(true);
    });
    byte buf[100];
    auto [n, err] = io::ReadFull(*r, buf);
    ASSERT_TRUE(!err || err == io::ErrUnexpectedEOF) << err;
    std::string result = str(std::span(buf, std::size_t(n)));
    done.recv();
    EXPECT_EQ(result, "hello");
    EXPECT_EQ(*writeErr, io::ErrClosedPipe);
    r->Close();
}

TEST(GoIO, PipeCloseError) {
    auto testError1 = errors::New("testError1");
    auto testError2 = errors::New("testError2");
    {
        auto [r, w] = io::Pipe();
        r->CloseWithError(testError1);
        EXPECT_EQ(w->Write({}).second, testError1);
        r->CloseWithError(testError2);
        EXPECT_EQ(w->Write({}).second, testError1);
    }
    {
        auto [r, w] = io::Pipe();
        w->CloseWithError(testError1);
        EXPECT_EQ(r->Read({}).second, testError1);
        w->CloseWithError(testError2);
        EXPECT_EQ(r->Read({}).second, testError1);
    }
}

TEST(GoIO, PipeConcurrent) {
    const std::string input = "0123456789abcdef";
    static constexpr int count = 8, readSize = 2;

    {  // Write
        auto [r, w] = io::Pipe();
        for (int i = 0; i < count; i++) {
            go([w, input] {
                time::Sleep(time::Millisecond);  // increase probability of race
                auto [n, err] = io::WriteString(*w, input);
                EXPECT_EQ(n, isize(input.size()));
                EXPECT_FALSE(err);
            });
        }
        std::vector<byte> buf(count * input.size());
        for (std::size_t i = 0; i < buf.size(); i += readSize) {
            auto [n, err] = r->Read(std::span(buf).subspan(i, readSize));
            EXPECT_EQ(n, readSize);
            EXPECT_FALSE(err);
        }
        std::string want;
        for (int i = 0; i < count; i++) want += input;
        EXPECT_EQ(str(buf), want);
    }

    {  // Read
        auto [r, w] = io::Pipe();
        constexpr int reads = count * 16 / readSize;
        auto c = makeChan<std::string>(reads);
        for (int i = 0; i < reads; i++) {
            go([r, c] {
                time::Sleep(time::Millisecond);  // increase probability of race
                byte buf[readSize];
                auto [n, err] = r->Read(buf);
                EXPECT_EQ(n, readSize);
                EXPECT_FALSE(err);
                c.send(str(buf));
            });
        }
        for (int i = 0; i < count; i++) {
            auto [n, err] = io::WriteString(*w, input);
            EXPECT_EQ(n, isize(input.size()));
            EXPECT_FALSE(err);
        }
        std::vector<std::string> got, want;
        for (int i = 0; i < reads; i++) got.push_back(c.recv());
        for (int i = 0; i < count; i++) {
            for (std::size_t j = 0; j < input.size(); j += readSize) want.push_back(input.substr(j, readSize));
        }
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        EXPECT_EQ(got, want);
    }
}

// A dropped half closes the pipe, so that the other side doesn't wait
// forever (Go's halves are garbage collected without closing).
TEST(GoIO, PipeDroppedHalf) {
    {
        auto [r, w] = io::Pipe();
        go([w = std::move(w)]() mutable { w = nullptr; });
        byte buf[8];
        EXPECT_EQ(r->Read(buf).second, io::ErrClosedPipe);
    }
    {
        auto [r, w] = io::Pipe();
        r = nullptr;
        EXPECT_EQ(io::WriteString(*w, "x").second, io::ErrClosedPipe);
    }
}

// ---------------------------------------------------------------------------
// MultiReader, MultiWriter

TEST(GoIO, MultiReader) {
    ptr<io::Reader> mr;
    std::vector<byte> storage;
    std::span<byte> buf;
    int nread = 0;
    auto withFooBar = [&](auto tests) {
        mr = io::MultiReader({strings::NewReader("foo "), strings::NewReader(""), strings::NewReader("bar")});
        storage.assign(20, 0);
        buf = storage;
        tests();
    };
    auto expectRead = [&](std::size_t size, std::string expected, error eerr) {
        nread++;
        auto [n, gerr] = mr->Read(buf.first(size));
        EXPECT_EQ(n, isize(expected.size())) << "#" << nread;
        EXPECT_EQ(str(buf.first(std::size_t(n))), expected) << "#" << nread;
        EXPECT_EQ(gerr, eerr) << "#" << nread << ": " << gerr;
        buf = buf.subspan(std::size_t(n));
    };
    withFooBar([&] {
        expectRead(2, "fo", nullptr);
        expectRead(5, "o ", nullptr);
        expectRead(5, "bar", nullptr);
        expectRead(5, "", io::EOF_);
    });
    withFooBar([&] {
        expectRead(4, "foo ", nullptr);
        expectRead(1, "b", nullptr);
        expectRead(3, "ar", nullptr);
        expectRead(1, "", io::EOF_);
    });
    withFooBar([&] { expectRead(5, "foo ", nullptr); });
}

TEST(GoIO, MultiReaderAsWriterTo) {
    auto mr = io::MultiReader({
        strings::NewReader("foo "),
        io::MultiReader({strings::NewReader(""), strings::NewReader("bar")}),  // tickle the buffer reusing codepath
    });
    auto mrAsWriterTo = dynamic_pointer_cast<io::WriterTo>(mr);
    ASSERT_TRUE(mrAsWriterTo) << "expected cast to WriterTo to succeed";
    strings::Builder sink;
    auto [n, err] = mrAsWriterTo->WriteTo(sink);
    ASSERT_FALSE(err);
    EXPECT_EQ(n, 7);
    EXPECT_EQ(sink.String(), "foo bar");
}

TEST(GoIO, MultiWriter) {
    auto sink = make_ptr<bytes::Buffer>();
    auto sha1 = make_ptr<bytes::Buffer>();
    auto mw = io::MultiWriter({sha1, sink});
    std::string sourceString = "My input text.";
    auto source = strings::NewReader(sourceString);
    auto [written, err] = io::Copy(*mw, *source);
    EXPECT_EQ(written, isize(sourceString.size()));
    EXPECT_FALSE(err);
    EXPECT_EQ(sink->String(), sourceString);
    EXPECT_EQ(sha1->String(), sourceString);
}

struct writerFunc final : io::Writer {
    std::function<std::pair<isize, error>(std::span<const byte>)> f;
    explicit writerFunc(decltype(f) fn) : f(std::move(fn)) {}
    std::pair<isize, error> Write(std::span<const byte> p) override { return f(p); }
};

// Test that a multiWriter writes to all writers until one fails.
TEST(GoIO, MultiWriterError) {
    auto f1 = make_ptr<writerFunc>([](std::span<const byte> p) { return std::pair<isize, error>{isize(p.size() / 2), io::ErrShortWrite}; });
    auto f2 = make_ptr<writerFunc>([](std::span<const byte> p) {
        ADD_FAILURE() << "MultiWriter called f2.Write";
        return std::pair<isize, error>{isize(p.size()), nullptr};
    });
    auto w = io::MultiWriter({f1, f2});
    byte buf[100] = {};
    auto [n, err] = w->Write(buf);
    EXPECT_EQ(n, 50);
    EXPECT_EQ(err, io::ErrShortWrite);
}

// Test that MultiWriter flattens chained multiWriters.
TEST(GoIO, MultiWriterSingleChainFlatten) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = io::MultiWriter({buf});
    for (int i = 0; i < 100; i++) w = io::MultiWriter({w});
    io::WriteString(*w, "hello");
    EXPECT_EQ(buf->String(), "hello");
}

TEST(GoIO, MultiReaderCopy) {
    std::vector<ptr<io::Reader>> s = {strings::NewReader("hello world")};
    auto r = io::MultiReader(s);
    s[0] = nullptr;
    auto [data, err] = io::ReadAll(*r);
    EXPECT_FALSE(err);
    EXPECT_EQ(str(data), "hello world");
}

TEST(GoIO, MultiWriterCopy) {
    auto buf = make_ptr<bytes::Buffer>();
    std::vector<ptr<io::Writer>> s = {buf};
    auto w = io::MultiWriter(s);
    s[0] = nullptr;
    auto [n, err] = io::WriteString(*w, "hello world");
    EXPECT_FALSE(err);
    EXPECT_EQ(n, 11);
    EXPECT_EQ(buf->String(), "hello world");
}

// A Reader that reads one byte and EOF at once in its Read call.
struct byteAndEOFReader final : io::Reader {
    byte b;
    explicit byteAndEOFReader(byte c) : b(c) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (p.empty()) panic("unexpected call");
        p[0] = b;
        return {1, io::EOF_};
    }
};

// This used to yield bytes forever; issue 16795.
TEST(GoIO, MultiReaderSingleByteWithEOF) {
    auto r = io::LimitReader(io::MultiReader({make_ptr<byteAndEOFReader>('a'), make_ptr<byteAndEOFReader>('b')}), 10);
    auto [got, err] = io::ReadAll(*r);
    ASSERT_FALSE(err);
    EXPECT_EQ(str(got), "ab");
}

// Test that a reader returning (n, EOF) at the end of a MultiReader chain
// continues to return EOF on its final read, rather than yielding a
// (0, EOF).
TEST(GoIO, MultiReaderFinalEOF) {
    auto r = io::MultiReader({bytes::NewReader({}), make_ptr<byteAndEOFReader>('a')});
    byte buf[2];
    auto [n, err] = r->Read(buf);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(err, io::EOF_);
}

TEST(GoIO, InterleavedMultiReader) {
    auto r1 = strings::NewReader("123");
    auto r2 = strings::NewReader("45678");
    auto mr1 = io::MultiReader({r1, r2});
    auto mr2 = io::MultiReader({mr1});
    byte buf[4];
    // Have mr2 use mr1's []Readers. Consume r1 (and return EOF like
    // bufio.Reader would to its caller).
    auto [n, err] = io::ReadFull(*mr2, buf);
    EXPECT_EQ(str(std::span(buf, std::size_t(n))), "1234");
    EXPECT_FALSE(err);
    // Consume the rest of r2 through mr1. This should not panic even
    // though mr2 cut off its readers.
    auto [n2, err2] = io::ReadFull(*mr1, buf);
    EXPECT_EQ(str(std::span(buf, std::size_t(n2))), "5678");
    EXPECT_FALSE(err2);
}

// ---------------------------------------------------------------------------
// io_test.go

TEST(GoIO, CopyBuffer) {
    auto rb = make_ptr<bytes::Buffer>();
    auto wb = make_ptr<bytes::Buffer>();
    rb->WriteString("hello, world.");
    // Hide the WriterTo and ReaderFrom, to use the buffer.
    struct onlyReader final : io::Reader {
        ptr<io::Reader> r;
        std::pair<isize, error> Read(std::span<byte> p) override { return r->Read(p); }
    };
    struct onlyWriter final : io::Writer {
        ptr<io::Writer> w;
        std::pair<isize, error> Write(std::span<const byte> p) override { return w->Write(p); }
    };
    onlyReader r;
    r.r = rb;
    onlyWriter w;
    w.w = wb;
    byte buf[1];
    io::CopyBuffer(w, r, buf);  // tiny buffer to keep it honest
    EXPECT_EQ(wb->String(), "hello, world.");
}

TEST(GoIO, TeeReader) {
    std::string src = "hello, world";
    std::vector<byte> dst(src.size());
    auto rb = bytes::NewBufferString(src);
    auto wb = make_ptr<bytes::Buffer>();
    auto r = io::TeeReader(rb, wb);
    {
        auto [n, err] = io::ReadFull(*r, dst);
        ASSERT_FALSE(err);
        ASSERT_EQ(n, isize(src.size()));
    }
    EXPECT_EQ(str(dst), src);
    EXPECT_EQ(wb->String(), src);
    {
        auto [n, err] = r->Read(dst);
        EXPECT_EQ(n, 0);
        EXPECT_EQ(err, io::EOF_);
    }
    rb = bytes::NewBufferString(src);
    auto [pr, pw] = io::Pipe();
    pr->Close();
    r = io::TeeReader(rb, pw);
    auto [n, err] = io::ReadFull(*r, dst);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(err, io::ErrClosedPipe);
}

TEST(GoIO, SectionReader_ReadAt) {
    const std::string dat = "a long sample data, 1234567890";
    const std::size_t L = dat.size();
    struct test {
        std::string data;
        int64 off, n;
        std::size_t bufLen;
        int64 at;
        std::string exp;
        error err;
    };
    std::vector<test> tests = {
        {"", 0, 10, 2, 0, "", io::EOF_},
        {dat, 0, int64(L), 0, 0, "", nullptr},
        {dat, int64(L), 1, 1, 0, "", io::EOF_},
        {dat, 0, int64(L) + 2, L, 0, dat, nullptr},
        {dat, 0, int64(L), L / 2, 0, dat.substr(0, L / 2), nullptr},
        {dat, 0, int64(L), L, 0, dat, nullptr},
        {dat, 0, int64(L), L / 2, 2, dat.substr(2, L / 2), nullptr},
        {dat, 3, int64(L), L / 2, 2, dat.substr(5, L / 2), nullptr},
        {dat, 3, int64(L / 2), L / 2 - 2, 2, dat.substr(5, L / 2 - 2), nullptr},
        {dat, 3, int64(L / 2), L / 2 + 2, 2, dat.substr(5, L / 2 - 2), io::EOF_},
        {dat, 0, 0, 0, -1, "", io::EOF_},
        {dat, 0, 0, 0, 1, "", io::EOF_},
    };
    int i = 0;
    for (const auto& tt : tests) {
        auto r = strings::NewReader(tt.data);
        auto s = io::NewSectionReader(r, tt.off, tt.n);
        std::vector<byte> buf(tt.bufLen);
        auto [n, err] = s->ReadAt(buf, tt.at);
        ASSERT_EQ(str(std::span(buf).first(std::size_t(n))), tt.exp) << i;
        ASSERT_EQ(err, tt.err) << i << ": " << err;
        auto [r2, off, n2] = s->Outer();
        ASSERT_EQ(r2.get(), static_cast<io::ReaderAt*>(r.get())) << i;
        ASSERT_EQ(off, tt.off) << i;
        ASSERT_EQ(n2, tt.n) << i;
        i++;
    }
}

TEST(GoIO, SectionReader_Seek) {
    // Verifies that NewSectionReader's Seeker behaves like bytes.NewReader
    // (which is like strings.NewReader).
    auto br = bytes::NewReader(toBytes("foo"));
    auto sr = io::NewSectionReader(br, 0, 3);
    for (int whence : {io::SeekStart, io::SeekCurrent, io::SeekEnd}) {
        for (int64 offset = -3; offset <= 4; offset++) {
            auto [brOff, brErr] = br->Seek(offset, whence);
            auto [srOff, srErr] = sr->Seek(offset, whence);
            EXPECT_EQ(bool(brErr), bool(srErr)) << "whence " << whence << ", offset " << offset;
            EXPECT_EQ(brOff, srOff) << "whence " << whence << ", offset " << offset;
        }
    }
    // And verify we can just seek past the end and get an EOF.
    auto [got, err] = sr->Seek(100, io::SeekStart);
    EXPECT_FALSE(err);
    EXPECT_EQ(got, 100);
    byte buf[10];
    auto [n, rerr] = sr->Read(buf);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(rerr, io::EOF_);
}

TEST(GoIO, SectionReader_Size) {
    for (auto [data, want] : std::vector<std::pair<std::string, int64>>{{"a long sample data, 1234567890", 30}, {"", 0}}) {
        auto sr = io::NewSectionReader(strings::NewReader(data), 0, int64(data.size()));
        EXPECT_EQ(sr->Size(), want);
    }
}

TEST(GoIO, SectionReader_Max) {
    auto r = strings::NewReader("abcdef");
    constexpr int64 maxint64 = std::numeric_limits<int64>::max();
    auto sr = io::NewSectionReader(r, 3, maxint64);
    byte buf[3];
    auto [n, err] = sr->Read(buf);
    EXPECT_EQ(n, 3);
    EXPECT_FALSE(err);
    auto [n2, err2] = sr->Read(buf);
    EXPECT_EQ(n2, 0);
    EXPECT_EQ(err2, io::EOF_);
    auto [r2, off, n3] = sr->Outer();
    EXPECT_EQ(r2.get(), static_cast<io::ReaderAt*>(r.get()));
    EXPECT_EQ(off, 3);
    EXPECT_EQ(n3, maxint64);
}

TEST(GoIO, OffsetWriter_Seek) {
    auto [tmpfile, err] = os::CreateTemp(os::TempDir(), "TestOffsetWriter_Seek");
    ASSERT_FALSE(err);
    struct closer {
        ptr<os::File> f;
        ~closer() {
            f->Close();
            os::Remove(f->Name());
        }
    } c{tmpfile};
    auto w = io::NewOffsetWriter(tmpfile, 0);

    // Should throw error errWhence if whence is not valid.
    {
        auto [_, e] = w->Seek(0, -1);
        ASSERT_TRUE(e);
        EXPECT_EQ(e.Error(), "Seek: invalid whence");
    }
    // Should throw error errOffset if offset is negative.
    {
        auto [_, e] = w->Seek(-1, io::SeekStart);
        ASSERT_TRUE(e);
        EXPECT_EQ(e.Error(), "Seek: invalid offset");
    }
    // Normal tests.
    struct test {
        int64 off;
        int whence;
        int64 returnOff;
    };
    for (auto [off, whence, returnOff] : std::vector<test>{
             {0, io::SeekStart, 0},     {1, io::SeekCurrent, 1}, {1, io::SeekCurrent, 2},
             {-1, io::SeekCurrent, 1}, {5, io::SeekStart, 5},   {-2, io::SeekCurrent, 3}}) {
        auto [got, e] = w->Seek(off, whence);
        ASSERT_FALSE(e);
        EXPECT_EQ(got, returnOff);
    }
}

TEST(GoIO, OffsetWriter_WriteAt) {
    const std::string content = "0123456789ABCDEF";
    struct test {
        int64 off;
        std::string want;
    };
    for (auto [off, want] : std::vector<test>{{0, "0123456789ABCDEF"}, {8, "\x00\x00\x00\x00\x00\x00\x00\x00" "01234567"}}) {
        auto [f, err] = os::CreateTemp(os::TempDir(), "TestOffsetWriter_WriteAt");
        ASSERT_FALSE(err);
        auto w = io::NewOffsetWriter(f, off);
        // Write two halves at once to check the state of the writer.
        auto [n1, e1] = w->WriteAt(as_bytes(content).first(8), 0);
        ASSERT_FALSE(e1);
        ASSERT_EQ(n1, 8);
        auto [n2, e2] = w->WriteAt(as_bytes(content).subspan(8), 8);
        ASSERT_FALSE(e2);
        ASSERT_EQ(n2, 8);
        f->Close();
        auto [got, rerr] = os::ReadFile(f->Name());
        os::Remove(f->Name());
        ASSERT_FALSE(rerr);
        std::string g = str(got);
        if (off == 0) {
            EXPECT_EQ(g, want);
        } else {
            EXPECT_EQ(g, std::string(8, '\0') + content);
        }
    }
}

TEST(GoIO, WriteAt_PositionPriorToBase) {
    auto [tmpfile, err] = os::CreateTemp(os::TempDir(), "TestOffsetWriter_WriteAt");
    ASSERT_FALSE(err);
    // start writing position in OffsetWriter
    auto w = io::NewOffsetWriter(tmpfile, 10);
    // position we want to write to the tmpfile
    auto [n, e] = w->WriteAt(as_bytes("hello"), -1);
    EXPECT_TRUE(e) << "want err, got nil";
    EXPECT_EQ(n, 0);
    tmpfile->Close();
    os::Remove(tmpfile->Name());
}

TEST(GoIO, OffsetWriter_Write) {
    const std::string content = "0123456789ABCDEF";
    auto [f, err] = os::CreateTemp(os::TempDir(), "TestOffsetWriter_Write");
    ASSERT_FALSE(err);
    auto w = io::NewOffsetWriter(f, 0);
    // Write a series of bytes in sequence and check that each byte is
    // written in the right place.
    for (std::size_t i = 0; i < content.size(); i++) {
        auto [n, e] = w->Write(as_bytes(content).subspan(i, 1));
        ASSERT_FALSE(e);
        ASSERT_EQ(n, 1);
    }
    f->Close();
    auto [got, rerr] = os::ReadFile(f->Name());
    os::Remove(f->Name());
    ASSERT_FALSE(rerr);
    EXPECT_EQ(str(got), content);
}

// go++: the io::RuneScanners (bufio::Reader, bytes::Reader, bytes::Buffer,
// strings::NewReader's) read UTF-8 runes, invalid bytes as RuneError of
// size 1, and unread the last one.
TEST(IO, RuneScanners) {
    const std::string text = "aé\xff世";
    std::vector<ptr<io::RuneScanner>> readers = {
        make_ptr<bufio::Reader>(strings::NewReader(string(text))),
        bytes::NewReader(toBytes(text)),
        bytes::NewBufferString(text),
        strings::NewReader(string(text)),
    };
    for (std::size_t k = 0; k < readers.size(); k++) {
        auto& r = *readers[k];
        std::vector<std::pair<rune, int>> got;
        for (;;) {
            auto [c, size, err] = r.ReadRune();
            if (err) {
                EXPECT_EQ(err, io::EOF_) << k;
                break;
            }
            got.push_back({c, size});
            if (got.size() == 2) {
                // Unread é and read it again.
                ASSERT_FALSE(r.UnreadRune()) << k;
                auto [c2, size2, err2] = r.ReadRune();
                EXPECT_EQ(c2, c) << k;
                EXPECT_EQ(size2, size) << k;
                EXPECT_FALSE(err2) << k;
            }
        }
        std::vector<std::pair<rune, int>> want = {{'a', 1}, {0xe9, 2}, {0xfffd, 1}, {0x4e16, 3}};
        EXPECT_EQ(got, want) << k;
    }
}

}  // namespace
