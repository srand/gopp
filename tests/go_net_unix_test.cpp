// Ported from Go 1.27.3's net/unixsock_test.go, unixsock_linux_test.go,
// unixsock_readmsg_test.go and the "unixgram" cases of packetconn_test.go,
// with Go's test names.
#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <ostream>
#include <string>
#include <vector>

using namespace gopp;

namespace gopp::net {

std::ostream& operator<<(std::ostream& os, const ptr<UnixAddr>& a) {
    if (!a) return os << "<nil>";
    return os << "&UnixAddr{Name:" << a->Name.view() << " Net:" << a->Net.view() << "}";
}

std::ostream& operator<<(std::ostream& os, const ptr<Addr>& a) {
    return os << (a ? a->String().view() : std::string_view("<nil>"));
}

}  // namespace gopp::net

namespace {

std::string str(const string& s) { return std::string(s.view()); }

// mockserver_test.go's testUnixAddr: a socket path in a new directory,
// removed with it when the test ends.
class TestDirs {
public:
    ~TestDirs() {
        for (auto& d : dirs_) os::RemoveAll(d);
    }
    std::string testUnixAddr() {
        auto [d, err] = os::MkdirTemp("", "");
        EXPECT_FALSE(err) << err;
        dirs_.push_back(str(d));
        return str(d) + "/sock";
    }

private:
    std::vector<std::string> dirs_;
};

bool unix_addr_equal(const ptr<net::Addr>& got, const ptr<net::UnixAddr>& want) {
    auto* g = dynamic_cast<net::UnixAddr*>(got.get());
    if (!g || !want) return !g && !want;
    return g->Name == want->Name && g->Net == want->Net;
}

ptr<net::UnixAddr> unix_addr(std::string name, std::string net) {
    return make_ptr<net::UnixAddr>(string(name), string(net));
}

bool isDeadlineExceeded(const error& err) { return errors::Is(err, os::ErrDeadlineExceeded); }

// syscall.Sendto with a SockaddrUnix, from a socket of its own.
void sendto_unix(const std::string& name, std::span<const byte> data) {
    int s = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, name.data(), std::min(name.size(), sizeof(sa.sun_path)));
    socklen_t len = static_cast<socklen_t>(sizeof(sa_family_t) + name.size() + 1);
    if (sa.sun_path[0] == '@' || sa.sun_path[0] == 0) {
        sa.sun_path[0] = 0;
        len--;
    }
    if (len > sizeof(sa)) len = sizeof(sa);
    EXPECT_GE(::sendto(s, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&sa), len), 0) << strerror(errno);
    ::close(s);
}

std::span<const byte> b(std::string_view s) { return as_bytes(s); }

}  // namespace

// ---------------------------------------------------------------------------
// unixsock_test.go

TEST(GoNetUnix, ReadUnixgramWithUnnamedSocket) {
    TestDirs t;
    std::string addr = t.testUnixAddr();
    auto [la, err] = net::ResolveUnixAddr("unixgram", addr);
    ASSERT_FALSE(err) << err;
    auto [c, lerr] = net::ListenUnixgram("unixgram", la);
    ASSERT_FALSE(lerr) << lerr;

    const byte data[5] = {1, 2, 3, 4, 5};
    auto off = make_chan<bool>();
    go([&] {
        sendto_unix(addr, data);
        off.send(true);
    });
    off.recv();

    std::vector<byte> buf(64);
    c->SetReadDeadline(time::Now().Add(100 * time::Millisecond));
    auto [n, from, rerr] = c->ReadFrom(buf);
    ASSERT_FALSE(rerr) << rerr;
    ASSERT_FALSE(from) << "unexpected peer address: " << from;
    ASSERT_TRUE(std::equal(buf.begin(), buf.begin() + n, std::begin(data), std::end(data)));
    c->Close();
    os::Remove(addr);
}

TEST(GoNetUnix, UnixgramZeroBytePayload) {
    TestDirs t;
    auto [c1, err] = net::ListenPacket("unixgram", t.testUnixAddr());
    ASSERT_FALSE(err) << err;
    auto [c2, derr] = net::Dial("unixgram", str(c1->LocalAddr()->String()));
    ASSERT_FALSE(derr) << derr;

    for (bool genericRead : {false, true}) {
        auto [n, werr] = c2->Write({});
        ASSERT_FALSE(werr) << werr;
        EXPECT_EQ(n, 0);
        c1->SetReadDeadline(time::Now().Add(100 * time::Millisecond));
        byte bb[1];
        ptr<net::Addr> peer;
        error rerr;
        if (genericRead) {
            rerr = dynamic_cast<net::Conn*>(c1.get())->Read(bb).second;
        } else {
            isize m;
            std::tie(m, peer, rerr) = c1->ReadFrom(bb);
        }
        if (!rerr) {
            ASSERT_FALSE(peer) << "unexpected peer address: " << peer;  // peer is connected-mode
        } else {
            ASSERT_TRUE(isDeadlineExceeded(rerr)) << rerr;  // Read may time out
        }
    }
    c2->Close();
    c1->Close();
}

TEST(GoNetUnix, UnixgramZeroByteBuffer) {
    // issue 4352: Recvfrom failed with "address family not
    // supported by protocol family" if zero-length buffer provided
    TestDirs t;
    auto [c1, err] = net::ListenPacket("unixgram", t.testUnixAddr());
    ASSERT_FALSE(err) << err;
    auto [c2, derr] = net::Dial("unixgram", str(c1->LocalAddr()->String()));
    ASSERT_FALSE(derr) << derr;

    auto bb = b("UNIXGRAM ZERO BYTE BUFFER TEST");
    for (bool genericRead : {false, true}) {
        auto [n, werr] = c2->Write(bb);
        ASSERT_FALSE(werr) << werr;
        EXPECT_EQ(n, static_cast<isize>(bb.size()));
        c1->SetReadDeadline(time::Now().Add(100 * time::Millisecond));
        ptr<net::Addr> peer;
        error rerr;
        if (genericRead) {
            rerr = dynamic_cast<net::Conn*>(c1.get())->Read({}).second;
        } else {
            isize m;
            std::tie(m, peer, rerr) = c1->ReadFrom({});
        }
        if (!rerr) {
            ASSERT_FALSE(peer) << "unexpected peer address: " << peer;
        } else {
            ASSERT_TRUE(isDeadlineExceeded(rerr)) << rerr;
        }
    }
    c2->Close();
    c1->Close();
}

namespace {

void expectErrWriteToConnected(const error& err) {
    ASSERT_TRUE(err) << "should fail";
    auto* e = errors::As<net::OpError>(err);
    ASSERT_TRUE(e && e->Err == net::ErrWriteToConnected) << "should fail as ErrWriteToConnected: " << err;
}

void testUnixgramWriteConn(const ptr<net::UnixAddr>& raddr) {
    auto [c, err] = net::Dial("unixgram", str(raddr->String()));
    ASSERT_FALSE(err) << err;
    auto* uc = dynamic_cast<net::UnixConn*>(c.get());
    auto bb = b("CONNECTED-MODE SOCKET");
    expectErrWriteToConnected(uc->WriteToUnix(bb, raddr).second);
    expectErrWriteToConnected(uc->WriteTo(bb, raddr).second);
    expectErrWriteToConnected(std::get<2>(uc->WriteMsgUnix(bb, {}, raddr)));
    ASSERT_FALSE(c->Write(bb).second);
    c->Close();
}

void testUnixgramWritePacketConn(TestDirs& t, const ptr<net::UnixAddr>& raddr) {
    std::string addr = t.testUnixAddr();
    auto [c, err] = net::ListenPacket("unixgram", addr);
    ASSERT_FALSE(err) << err;
    auto* uc = dynamic_cast<net::UnixConn*>(c.get());
    auto bb = b("UNCONNECTED-MODE SOCKET");
    ASSERT_FALSE(uc->WriteToUnix(bb, raddr).second);
    ASSERT_FALSE(c->WriteTo(bb, raddr).second);
    ASSERT_FALSE(std::get<2>(uc->WriteMsgUnix(bb, {}, raddr)));
    ASSERT_TRUE(uc->Write(bb).second) << "should fail";
    c->Close();
    os::Remove(addr);
}

}  // namespace

TEST(GoNetUnix, UnixgramWrite) {
    TestDirs t;
    std::string addr = t.testUnixAddr();
    auto [laddr, err] = net::ResolveUnixAddr("unixgram", addr);
    ASSERT_FALSE(err) << err;
    auto [c, lerr] = net::ListenPacket("unixgram", addr);
    ASSERT_FALSE(lerr) << lerr;
    testUnixgramWriteConn(laddr);
    testUnixgramWritePacketConn(t, laddr);
    c->Close();
    os::Remove(addr);
}

TEST(GoNetUnix, UnixConnLocalAndRemoteNames) {
    TestDirs t;
    for (std::string laddr : {std::string(), t.testUnixAddr()}) {
        std::string taddr = t.testUnixAddr();
        auto [ta, err] = net::ResolveUnixAddr("unix", taddr);
        ASSERT_FALSE(err) << err;
        auto [ln, lerr] = net::ListenUnix("unix", ta);
        ASSERT_FALSE(lerr) << lerr;

        auto [la, rerr] = net::ResolveUnixAddr("unix", laddr);
        ASSERT_FALSE(rerr) << rerr;
        auto [c, derr] = net::DialUnix("unix", la, ta);
        ASSERT_FALSE(derr) << derr;
        ASSERT_FALSE(c->Write(b("UNIXCONN LOCAL AND REMOTE NAME TEST")).second);

        if (laddr.empty()) laddr = "@";  // autobind feature
        struct {
            ptr<net::Addr> got;
            ptr<net::UnixAddr> want;
        } connAddrs[] = {
            {ln->Addr(), ta},
            {c->LocalAddr(), unix_addr(laddr, "unix")},
            {c->RemoteAddr(), ta},
        };
        for (auto& ca : connAddrs) {
            ASSERT_TRUE(unix_addr_equal(ca.got, ca.want)) << "got " << ca.got << ", expected " << ca.want;
        }
        c->Close();
        ln->Close();
    }
}

TEST(GoNetUnix, UnixgramConnLocalAndRemoteNames) {
    TestDirs t;
    for (std::string laddr : {std::string(), t.testUnixAddr()}) {
        std::string taddr = t.testUnixAddr();
        auto [ta, err] = net::ResolveUnixAddr("unixgram", taddr);
        ASSERT_FALSE(err) << err;
        auto [c1, lerr] = net::ListenUnixgram("unixgram", ta);
        ASSERT_FALSE(lerr) << lerr;

        ptr<net::UnixAddr> la;
        if (!laddr.empty()) {
            error rerr;
            std::tie(la, rerr) = net::ResolveUnixAddr("unixgram", laddr);
            ASSERT_FALSE(rerr) << rerr;
        }
        auto [c2, derr] = net::DialUnix("unixgram", la, ta);
        ASSERT_FALSE(derr) << derr;

        if (laddr.empty()) laddr = "@";  // autobind feature
        struct {
            ptr<net::Addr> got;
            ptr<net::UnixAddr> want;
        } connAddrs[] = {
            {c1->LocalAddr(), ta},
            {c1->RemoteAddr(), nullptr},
            {c2->LocalAddr(), unix_addr(laddr, "unixgram")},
            {c2->RemoteAddr(), ta},
        };
        for (auto& ca : connAddrs) {
            ASSERT_TRUE(unix_addr_equal(ca.got, ca.want)) << "got " << ca.got << "; want " << ca.want;
        }
        c2->Close();
        c1->Close();
        os::Remove(taddr);
    }
}

TEST(GoNetUnix, UnixUnlink) {
    TestDirs t;
    std::string name = t.testUnixAddr();

    auto listen = [&] {
        auto [l, err] = net::Listen("unix", name);
        EXPECT_FALSE(err) << err;
        return ptr<net::UnixListener>(dynamic_cast<net::UnixListener*>(l.get()));
    };
    auto fileListener = [&](const ptr<net::UnixListener>& l) {
        auto [f, err] = l->File();
        EXPECT_FALSE(err) << err;
        auto [ln, ferr] = net::FileListener(f);
        EXPECT_FALSE(ferr) << ferr;
        return std::pair{f, ln};
    };
    auto exists = [&] { return !os::Stat(name).second; };

    {
        SCOPED_TRACE("Listener should remove on close");
        auto l = listen();
        ASSERT_TRUE(exists()) << "unix socket does not exist after Listen";
        l->Close();
        ASSERT_FALSE(exists()) << "unix socket does exist after Listener close";
    }
    {
        SCOPED_TRACE("FileListener should not");
        auto l = listen();
        auto [f, l1] = fileListener(l);
        ASSERT_TRUE(exists()) << "after FileListener";
        f->Close();
        ASSERT_TRUE(exists()) << "after File close";
        l1->Close();
        ASSERT_TRUE(exists()) << "after FileListener close";
        l->Close();
        ASSERT_FALSE(exists()) << "after Listener close";
    }
    {
        SCOPED_TRACE("Only first call to l.Close should remove");
        auto l = listen();
        ASSERT_TRUE(exists()) << "after Listen";
        l->Close();
        ASSERT_FALSE(exists()) << "after Listener close";
        ASSERT_FALSE(os::WriteFile(name, std::string_view("hello world"), 0666)) << "cannot recreate socket file";
        ASSERT_TRUE(exists()) << "after writing temp file";
        l->Close();
        ASSERT_TRUE(exists()) << "after second Listener close";
        os::Remove(name);
    }
    {
        SCOPED_TRACE("Listen/SetUnlinkOnClose(true)");
        auto l = listen();
        ASSERT_TRUE(exists());
        l->SetUnlinkOnClose(true);
        l->Close();
        ASSERT_FALSE(exists()) << "after Listener close";
    }
    {
        SCOPED_TRACE("Listen/SetUnlinkOnClose(false)");
        auto l = listen();
        ASSERT_TRUE(exists());
        l->SetUnlinkOnClose(false);
        l->Close();
        ASSERT_TRUE(exists()) << "after Listener close";
        os::Remove(name);
    }
    {
        SCOPED_TRACE("FileListener/SetUnlinkOnClose(true)");
        auto l = listen();
        auto [f, l1] = fileListener(l);
        ASSERT_TRUE(exists());
        dynamic_cast<net::UnixListener*>(l1.get())->SetUnlinkOnClose(true);
        f->Close();
        ASSERT_TRUE(exists()) << "after File close";
        l1->Close();
        ASSERT_FALSE(exists()) << "after FileListener close";
        l->Close();
    }
    {
        SCOPED_TRACE("FileListener/SetUnlinkOnClose(false)");
        auto l = listen();
        auto [f, l1] = fileListener(l);
        ASSERT_TRUE(exists());
        dynamic_cast<net::UnixListener*>(l1.get())->SetUnlinkOnClose(false);
        f->Close();
        ASSERT_TRUE(exists()) << "after File close";
        l1->Close();
        ASSERT_TRUE(exists()) << "after FileListener close";
        l->Close();
    }
}

// Ensure UnixConn read methods return io.EOF directly instead of wrapping it
// in net.OpError, per the io.Reader contract. See issue #78137.
TEST(GoNetUnix, UnixConnReadEOF) {
    TestDirs t;
    std::string path = t.testUnixAddr();
    auto addr = unix_addr(path, "unix");

    auto run = [&](auto read) {
        auto [ln, err] = net::ListenUnix("unix", addr);
        ASSERT_FALSE(err) << err;
        go([ln] {
            auto [srv, aerr] = ln->AcceptUnix();
            EXPECT_FALSE(aerr) << aerr;
            if (srv) srv->Close();
        });
        auto [cl, derr] = net::DialUnix("unix", nullptr, addr);
        ASSERT_FALSE(derr) << derr;
        error rerr = read(*cl);
        EXPECT_EQ(rerr, io::EOF_) << rerr;
        cl->Close();
        ln->Close();
    };
    {
        SCOPED_TRACE("ReadMsgUnix");
        run([](net::UnixConn& c) {
            byte buf[1];
            return std::get<4>(c.ReadMsgUnix(buf, {}));
        });
    }
    {
        SCOPED_TRACE("ReadFromUnix");
        run([](net::UnixConn& c) {
            byte buf[1];
            return std::get<2>(c.ReadFromUnix(buf));
        });
    }
    {
        SCOPED_TRACE("ReadFrom");
        run([](net::UnixConn& c) {
            byte buf[1];
            return std::get<2>(c.ReadFrom(buf));
        });
    }
}

// ---------------------------------------------------------------------------
// unixsock_linux_test.go

TEST(GoNetUnix, UnixgramAutobind) {
    auto laddr = unix_addr("", "unixgram");
    auto [c1, err] = net::ListenUnixgram("unixgram", laddr);
    ASSERT_FALSE(err) << err;

    // retrieve the autobind address
    auto autoAddr = ptr<net::UnixAddr>(dynamic_cast<net::UnixAddr*>(c1->LocalAddr().get()));
    ASSERT_TRUE(autoAddr && autoAddr->Name.size() > 1) << "invalid autobind address: " << autoAddr;
    ASSERT_EQ(autoAddr->Name.view()[0], '@') << "invalid autobind address: " << autoAddr;

    auto [c2, derr] = net::DialUnix("unixgram", nullptr, autoAddr);
    ASSERT_FALSE(derr) << derr;
    ASSERT_TRUE(unix_addr_equal(c2->RemoteAddr(), autoAddr))
        << "expected autobind address " << c1->LocalAddr() << ", got " << c2->RemoteAddr();
    c2->Close();
    c1->Close();
}

TEST(GoNetUnix, UnixAutobindClose) {
    auto laddr = unix_addr("", "unix");
    auto [ln, err] = net::ListenUnix("unix", laddr);
    ASSERT_FALSE(err) << err;
    ln->Close();
}

TEST(GoNetUnix, UnixAbstractLongNameNulStart) {
    // An abstract socket name that starts with a null byte ("\x00") whose
    // length is the maximum of RawSockaddrUnix Path len.
    std::string paddedAddr(sizeof(sockaddr_un::sun_path), '\0');
    std::memcpy(paddedAddr.data(), "\0abstract_test", 14);

    auto [la, err] = net::ResolveUnixAddr("unix", paddedAddr);
    ASSERT_FALSE(err) << err;
    auto [c, lerr] = net::ListenUnix("unix", la);
    ASSERT_FALSE(lerr) << lerr;
    c->Close();
}

TEST(GoNetUnix, UnixgramLinuxAbstractLongName) {
    // An abstract socket name whose length is exactly the maximum
    // RawSockaddrUnix Path len.
    std::string addr(sizeof(sockaddr_un::sun_path), '\0');
    std::memcpy(addr.data(), "@abstract_test", 14);

    auto [la, err] = net::ResolveUnixAddr("unixgram", addr);
    ASSERT_FALSE(err) << err;
    auto [c, lerr] = net::ListenUnixgram("unixgram", la);
    ASSERT_FALSE(lerr) << lerr;

    const byte data[5] = {1, 2, 3, 4, 5};
    auto off = make_chan<bool>();
    go([&] {
        sendto_unix(addr, data);
        off.send(true);
    });
    off.recv();

    std::vector<byte> buf(64);
    c->SetReadDeadline(time::Now().Add(100 * time::Millisecond));
    auto [n, from, rerr] = c->ReadFrom(buf);
    ASSERT_FALSE(rerr) << rerr;
    ASSERT_FALSE(from) << "unexpected peer address: " << from;
    ASSERT_TRUE(std::equal(buf.begin(), buf.begin() + n, std::begin(data), std::end(data)));
    c->Close();
}

// ---------------------------------------------------------------------------
// unixsock_readmsg_test.go

TEST(GoNetUnix, UnixConnReadMsgUnixSCMRightsCloseOnExec) {
    auto [scmFile, err] = os::Open(os::DevNull);
    ASSERT_FALSE(err) << "file open: " << err;

    auto rights = syscall::UnixRights({static_cast<int>(scmFile->Fd())});
    auto [fds, serr] = syscall::Socketpair(AF_LOCAL, SOCK_STREAM, 0);
    ASSERT_FALSE(serr) << "Socketpair: " << serr;

    auto writeFile = os::NewFile(static_cast<uintptr>(fds[0]), string::from_static("write-socket"));
    auto readFile = os::NewFile(static_cast<uintptr>(fds[1]), string::from_static("read-socket"));

    auto [cw, cwerr] = net::FileConn(writeFile);
    ASSERT_FALSE(cwerr) << "FileConn: " << cwerr;
    auto [cr, crerr] = net::FileConn(readFile);
    ASSERT_FALSE(crerr) << "FileConn: " << crerr;

    auto* ucw = dynamic_cast<net::UnixConn*>(cw.get());
    ASSERT_TRUE(ucw) << "want UnixConn";
    auto* ucr = dynamic_cast<net::UnixConn*>(cr.get());
    ASSERT_TRUE(ucr) << "want UnixConn";

    std::vector<byte> oob(static_cast<std::size_t>(syscall::CmsgSpace(4)));
    ASSERT_FALSE(ucw->SetWriteDeadline(time::Now().Add(5 * time::Second)));
    auto [wn, woobn, werr] = ucw->WriteMsgUnix({}, rights, nullptr);
    ASSERT_FALSE(werr) << "UnixConn readMsg: " << werr;
    ASSERT_FALSE(ucr->SetReadDeadline(time::Now().Add(5 * time::Second)));
    auto [rn, oobn, flags, from, rerr] = ucr->ReadMsgUnix({}, oob);
    ASSERT_FALSE(rerr) << "UnixConn readMsg: " << rerr;

    auto [scms, perr] = syscall::ParseSocketControlMessage(std::span(oob).first(static_cast<std::size_t>(oobn)));
    ASSERT_FALSE(perr) << "ParseSocketControlMessage: " << perr;
    ASSERT_EQ(scms.size(), 1u) << "expected 1 SocketControlMessage";
    auto [gotFDs, uerr] = syscall::ParseUnixRights(scms[0]);
    ASSERT_FALSE(uerr) << "syscall.ParseUnixRights: " << uerr;
    ASSERT_EQ(gotFDs.size(), 1u) << "wanted only 1 fd";

    int fl = ::fcntl(gotFDs[0], F_GETFD, 0);
    EXPECT_TRUE(fl >= 0 && (fl & FD_CLOEXEC)) << "got flags " << fl << ", want FD_CLOEXEC set";
    EXPECT_EQ(::close(gotFDs[0]), 0);

    cr->Close();
    cw->Close();
    readFile->Close();
    writeFile->Close();
    scmFile->Close();
}

// ---------------------------------------------------------------------------
// packetconn_test.go, "unixgram"

TEST(GoNetUnix, PacketConn) {
    TestDirs t;
    std::string addr1 = t.testUnixAddr(), addr2 = t.testUnixAddr();
    auto wb = b("PACKETCONN TEST");
    auto [c1, err] = net::ListenPacket("unixgram", addr1);
    ASSERT_FALSE(err) << err;
    auto [c2, err2] = net::ListenPacket("unixgram", addr2);
    ASSERT_FALSE(err2) << err2;
    std::vector<byte> rb1(128), rb2(128);
    ASSERT_FALSE(c1->WriteTo(wb, c2->LocalAddr()).second);
    ASSERT_FALSE(std::get<2>(c2->ReadFrom(rb2)));
    ASSERT_FALSE(c2->WriteTo(wb, c1->LocalAddr()).second);
    ASSERT_FALSE(std::get<2>(c1->ReadFrom(rb1)));
    c1->Close();
    c2->Close();
}

TEST(GoNetUnix, ConnAndPacketConn) {
    TestDirs t;
    auto wb = b("PACKETCONN TEST");
    auto [c1, err] = net::ListenPacket("unixgram", t.testUnixAddr());
    ASSERT_FALSE(err) << err;
    auto [c2, derr] = net::Dial("unixgram", str(c1->LocalAddr()->String()));
    ASSERT_FALSE(derr) << derr;
    c2->LocalAddr();
    c2->RemoteAddr();
    ASSERT_FALSE(c2->Write(wb).second);
    std::vector<byte> rb1(128);
    ASSERT_FALSE(std::get<2>(c1->ReadFrom(rb1)));
    // Go stops here for "unixgram": the dialing side is unnamed.
    c2->Close();
    c1->Close();
}
