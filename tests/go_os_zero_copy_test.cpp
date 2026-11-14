// Ported from Go 1.27.3's os/readfrom_unix_test.go (TestCopyFile),
// readfrom_linux_test.go (TestSpliceFile, TestProcCopy) and
// writeto_linux_test.go (TestSendFile), with Go's test names and sizes.
//
// Adapted: Go's tests hook poll.CopyFileRange, poll.Splice and
// poll.SendFile to check which descriptors they got; these check the data,
// the offsets and the limits that come out. The splice-to-TTY case is left
// out (it needs a pseudo-terminal).
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <random>
#include <string>
#include <vector>

using namespace gopp;

namespace {

const std::vector<int64> sizes = {1, 42, 1025, int64(::getpagesize()) + 1, 32769};

std::vector<byte> randomData(int64 size) {
    std::mt19937 prng(std::random_device{}());
    std::vector<byte> data(static_cast<std::size_t>(size));
    for (auto& b : data) b = byte(prng());
    return data;
}

// readfrom_unix_test.go's createTempFile: a file of random data, at its
// start.
std::pair<ptr<os::File>, std::vector<byte>> createTempFile(const std::string& dir, const std::string& name,
                                                           int64 size) {
    auto [f, err] = os::CreateTemp(dir, name);
    EXPECT_FALSE(err) << err;
    auto data = randomData(size);
    f->Write(data);
    f->Sync();
    f->Seek(0, io::SeekStart);
    return {f, data};
}

std::string tempDir() {
    auto [d, err] = os::MkdirTemp("", "gopp-zerocopy");
    EXPECT_FALSE(err);
    return std::string(d.view());
}

void mustContainData(os::File& f, std::span<const byte> data) {
    std::vector<byte> got(data.size());
    auto [n, err] = io::ReadFull(f, got);
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(std::equal(got.begin(), got.end(), data.begin())) << "didn't get the same data back from "
                                                                  << f.Name().view();
    byte b[1];
    EXPECT_EQ(f.Read(b).second, io::EOF_) << "not at EOF";
}

void mustSeekStart(os::File& f) { ASSERT_FALSE(f.Seek(0, io::SeekStart).second); }

// readfrom_unix_test.go's testCopyFile.
void testCopyFile(os::File& dst, const ptr<os::File>& src, std::vector<byte> data, int64 limit) {
    int64 n;
    error err;
    std::unique_ptr<io::LimitedReader> lr;
    if (limit >= 0) {
        lr = std::make_unique<io::LimitedReader>(src, limit);
        if (limit < int64(data.size())) data.resize(std::size_t(limit));
        std::tie(n, err) = io::Copy(dst, *lr);
    } else {
        std::tie(n, err) = io::Copy(dst, *src);
    }
    ASSERT_FALSE(err) << err;
    auto [dstoff, e1] = dst.Seek(0, io::SeekCurrent);
    ASSERT_FALSE(e1);
    auto [srcoff, e2] = src->Seek(0, io::SeekCurrent);
    ASSERT_FALSE(e2);
    EXPECT_EQ(dstoff, srcoff) << "offsets differ";
    EXPECT_EQ(dstoff, int64(data.size()));
    EXPECT_EQ(n, int64(data.size())) << "short ReadFrom";
    mustSeekStart(dst);
    mustContainData(dst, data);
    if (lr) { EXPECT_EQ(lr->N, limit - n) << "didn't update limit correctly"; }
}

void testCopyFiles(int64 size, int64 limit) {
    std::string dir = tempDir();
    auto [src, data] = createTempFile(dir, "test-copy-file-src", size);
    auto [dst, err] = os::CreateTemp(dir, "test-copy-file-dst");
    ASSERT_FALSE(err);
    testCopyFile(*dst, src, data, limit);
    dst->Close();
    src->Close();
    os::RemoveAll(dir);
}

// A connected pair of stream sockets (Go's createSocketPair).
std::pair<ptr<net::Conn>, ptr<net::Conn>> createSocketPair(const std::string& proto, const std::string& dir) {
    std::string addr = proto == "tcp" ? "127.0.0.1:0" : dir + "/sock";
    auto [ln, err] = net::Listen(proto, addr);
    EXPECT_FALSE(err) << err;
    auto accepted = make_chan<ptr<net::Conn>>(1);
    go([ln, accepted] { accepted.send(ln->Accept().first); });
    auto [client, derr] = net::Dial(proto, ln->Addr()->String().view());
    EXPECT_FALSE(derr) << derr;
    auto server = accepted.recv();
    ln->Close();
    return {client, server};
}

// readfrom_linux_test.go's testSpliceFile: from a socket into a file.
void testSpliceFile(const std::string& proto, int64 size, int64 limit) {
    std::string dir = tempDir();
    auto [dst, err] = os::CreateTemp(dir, "test-splice-file-dst");
    ASSERT_FALSE(err);
    auto [client, server] = createSocketPair(proto, dir);
    auto data = randomData(size);
    go([client = client, data] {
        client->Write(data);
        client->Close();
    });
    int64 n;
    error cerr;
    std::unique_ptr<io::LimitedReader> lr;
    auto want = data;
    if (limit >= 0) {
        lr = std::make_unique<io::LimitedReader>(server, limit);
        if (limit < int64(want.size())) want.resize(std::size_t(limit));
        std::tie(n, cerr) = io::Copy(*dst, *lr);
    } else {
        std::tie(n, cerr) = io::Copy(*dst, *server);
    }
    ASSERT_FALSE(cerr) << cerr;
    auto [dstoff, e1] = dst->Seek(0, io::SeekCurrent);
    ASSERT_FALSE(e1);
    EXPECT_EQ(dstoff, int64(want.size()));
    EXPECT_EQ(n, int64(want.size())) << "short ReadFrom";
    mustSeekStart(*dst);
    mustContainData(*dst, want);
    if (lr) { EXPECT_EQ(lr->N, limit - n) << "didn't update limit correctly"; }
    server->Close();
    dst->Close();
    os::RemoveAll(dir);
}

// writeto_linux_test.go's testSendFile: from a file to a socket.
void testSendFile(const std::string& proto, int64 size) {
    std::string dir = tempDir();
    auto [client, server] = createSocketPair(proto, dir);
    auto [src, data] = createTempFile(dir, "writeto-sendfile-to-socket", size);
    auto [n, err] = io::Copy(*client, *src);
    ASSERT_FALSE(err) << err;
    std::vector<byte> dstData(data.size());
    auto [m, rerr] = io::ReadFull(*server, dstData);
    ASSERT_FALSE(rerr) << rerr;
    EXPECT_EQ(n, int64(data.size())) << "data length mismatch for io.Copy";
    EXPECT_EQ(m, isize(data.size())) << "data length mismatch for net.Conn.Read";
    EXPECT_EQ(dstData, data) << "data mismatch";
    client->Close();
    server->Close();
    src->Close();
    os::RemoveAll(dir);
}

}  // namespace

TEST(GoOsZeroCopy, CopyFile) {
    // Basic
    for (auto size : sizes) {
        SCOPED_TRACE("Basic/" + std::to_string(size));
        testCopyFiles(size, -1);
    }
    // Limited
    for (auto size : sizes) {
        SCOPED_TRACE("Limited/" + std::to_string(size));
        testCopyFiles(size, size - 1);  // OneLess
        testCopyFiles(size, size / 2);  // Half
        testCopyFiles(size, size + 7);  // More
    }
}

TEST(GoOsZeroCopy, CopyFileDoesntTryInAppendMode) {
    std::string dir = tempDir();
    auto [src, data] = createTempFile(dir, "test-copy-file-src", 42);
    auto [dst, err] = os::CreateTemp(dir, "test-copy-file-dst");
    ASSERT_FALSE(err);
    auto [dst2, oerr] = os::OpenFile(dst->Name().view(), os::O_RDWR_ | os::O_APPEND_, 0755);
    ASSERT_FALSE(oerr) << oerr;
    auto [n, cerr] = io::Copy(*dst2, *src);
    ASSERT_FALSE(cerr) << cerr;
    mustSeekStart(*dst2);
    mustContainData(*dst2, data);  // through traditional means
    dst2->Close();
    dst->Close();
    src->Close();
    os::RemoveAll(dir);
}

TEST(GoOsZeroCopy, CopyFileItself) {
    std::string dir = tempDir();
    auto [f, err] = os::CreateTemp(dir, "file-readfrom-itself-test");
    ASSERT_FALSE(err);
    std::string data = "hello world!";
    ASSERT_FALSE(f->Write(as_bytes(data)).second);
    ASSERT_FALSE(f->Sync());
    ASSERT_FALSE(f->Seek(0, io::SeekStart).second);
    // Reading the file into itself: no zero-copy, and no error.
    auto [n, cerr] = io::Copy(*f, *f);
    EXPECT_FALSE(cerr) << cerr;
    f->Close();
    os::RemoveAll(dir);
}

TEST(GoOsZeroCopy, SpliceFile) {
    for (std::string proto : {"tcp", "unix"}) {
        for (auto size : sizes) {
            SCOPED_TRACE("Basic-" + proto + "/" + std::to_string(size));
            testSpliceFile(proto, size, -1);
        }
        for (auto size : sizes) {
            SCOPED_TRACE("Limited-" + proto + "/" + std::to_string(size));
            testSpliceFile(proto, size, size - 1);  // OneLess
            testSpliceFile(proto, size, size / 2);  // Half
            testSpliceFile(proto, size, size + 1);  // More
        }
    }
}

TEST(GoOsZeroCopy, ProcCopy) {
    const std::string cmdlineFile = "/proc/self/cmdline";
    auto [cmdline, err] = os::ReadFile(cmdlineFile);
    if (err) GTEST_SKIP() << "can't read /proc file: " << err;
    auto [in, oerr] = os::Open(cmdlineFile);
    ASSERT_FALSE(oerr);
    std::string dir = tempDir();
    std::string outFile = dir + "/cmdline";
    auto [out, cerr] = os::Create(outFile);
    ASSERT_FALSE(cerr);
    auto [n, copyErr] = io::Copy(*out, *in);
    ASSERT_FALSE(copyErr) << copyErr;
    ASSERT_FALSE(out->Close());
    auto [copy, rerr] = os::ReadFile(outFile);
    ASSERT_FALSE(rerr);
    EXPECT_EQ(asStringView(copy), asStringView(cmdline));
    in->Close();
    os::RemoveAll(dir);
}

TEST(GoOsZeroCopy, SendFile) {
    for (std::string proto : {"unix", "tcp"}) {
        for (auto size : sizes) {
            SCOPED_TRACE("sendfile-to-" + proto + "/" + std::to_string(size));
            testSendFile(proto, size);
        }
    }
}

// go++'s own: a file's SyscallConn gives its descriptor.
TEST(GoOsZeroCopy, FileSyscallConn) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err);
    auto [rc, serr] = w->SyscallConn();
    ASSERT_FALSE(serr);
    int got = -1;
    ASSERT_FALSE(rc->Control([&](uintptr fd) { got = int(fd); }));
    EXPECT_GE(got, 0);
    // A write through the raw descriptor reaches the reader.
    ASSERT_FALSE(rc->Write([&](uintptr fd) { return ::write(int(fd), "x", 1) == 1; }));
    byte b[1];
    auto [n, rerr] = r->Read(b);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(b[0], 'x');
    w->Close();
    EXPECT_TRUE(rc->Control([](uintptr) {})) << "Control after Close should fail";
    r->Close();
}
