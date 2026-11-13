// Ported from Go 1.27.3's os tests: pipe_test.go and the pipe tests of
// os_unix_test.go, with Go's test names. See go_os_test.cpp.
//
// The tests that run the test program as a child (TestStdPipe,
// TestReadNonblockingFd) start it with fork and exec, with the child's
// part selected by the environment, as Go's do.
#include "go_os_test_util.hpp"

#include <gopp/bufio.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os/signal.hpp>

#include <signal.h>
#include <sys/wait.h>

#include <cstdlib>

using namespace os_test;

namespace {

// Runs this test program's test `name` as a child with the extra
// environment variables, its descriptors 1, 2 and 3 set to the given ones
// (-1 keeps the descriptor; for 0, stdin), and returns its wait status.
int run_child(const std::string& name, const std::vector<std::string>& env, int in, int out, int err, int fd3) {
    std::string filter = "--gtest_filter=" + name;
    std::vector<std::string> envs;
    for (char** e = environ; *e; e++) envs.emplace_back(*e);
    for (auto& e : env) envs.push_back(e);
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    char self[] = "/proc/self/exe";
    char* argv[] = {self, filter.data(), nullptr};
    pid_t pid = ::fork();
    if (pid == 0) {
        if (in >= 0) ::dup2(in, 0);
        if (out >= 0) ::dup2(out, 1);
        if (err >= 0) ::dup2(err, 2);
        if (fd3 >= 0) ::dup2(fd3, 3);
        // Keep the descriptors open in the child, as exec.Cmd does.
        for (int fd : {0, 1, 2, 3}) ::fcntl(fd, F_SETFD, 0);
        ::execve(self, argv, envp.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

const char* test_full_name() {
    static std::string s;
    auto* t = testing::UnitTest::GetInstance()->current_test_info();
    s = std::string(t->test_suite_name()) + "." + t->name();
    return s.c_str();
}

}  // namespace

TEST(GoOs, EPIPE) {
    // Check that EPIPE is returned when writing to a broken pipe, instead
    // of the process dying of SIGPIPE.
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(r->Close());
    for (int i = 0; i < 20; i++) {
        auto [_, werr] = w->Write(b("hi"));
        ASSERT_TRUE(werr) << "unexpected success of Write to broken pipe";
        error e = werr;
        if (auto* pe = dynamic_cast<const fs::PathError*>(e.get())) e = pe->Err;
        if (auto* se = dynamic_cast<const os::SyscallError*>(e.get())) e = se->Err;
        EXPECT_TRUE(is_errno(e, EPIPE)) << "iteration " << i << ": got " << e << ", expected EPIPE";
    }
    w->Close();
}

TEST(GoOs, StdPipe) {
    if (const char* helper = std::getenv("GO_TEST_STD_PIPE_HELPER")) {
        if (std::getenv("GO_TEST_STD_PIPE_HELPER_SIGNAL")) {
            auto c = make_chan<os::Signal>(1);
            os::signal::Notify(c, syscall::SIGPIPE_);
        }
        switch (std::atoi(helper)) {
        case 1: os::Stdout.Write(b("stdout")); break;
        case 2: os::Stderr.Write(b("stderr")); break;
        case 3:
            if (!os::NewFile(3, "3")->Write(b("3")).second) os::Exit(3);
            break;
        default: std::abort();
        }
        os::Exit(0);
    }

    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(r->Close());
    int wfd = static_cast<int>(w->Fd());
    // Invoke the test program to run the test and write to a closed pipe.
    // If sig is false: writing to stdout or stderr should cause an
    // immediate SIGPIPE; writing to descriptor 3 should fail with EPIPE
    // and then exit 0. If sig is true: all writes should fail with EPIPE
    // and then exit 0.
    for (bool sig : {false, true}) {
        for (int dest = 1; dest < 4; dest++) {
            std::vector<std::string> env = {"GO_TEST_STD_PIPE_HELPER=" + std::to_string(dest)};
            if (sig) env.push_back("GO_TEST_STD_PIPE_HELPER_SIGNAL=1");
            int ws = run_child(test_full_name(), env, -1, wfd, wfd, wfd);
            if (WIFEXITED(ws) && WEXITSTATUS(ws) == 0) {
                if (!sig && dest < 3) {
                    ADD_FAILURE() << "unexpected success of write to closed pipe " << dest << " sig " << sig << " in child";
                }
            } else if (WIFSIGNALED(ws) && WTERMSIG(ws) == SIGPIPE) {
                if (sig || dest > 2) {
                    ADD_FAILURE() << "unexpected SIGPIPE signal for descriptor " << dest << " sig " << sig;
                }
            } else {
                ADD_FAILURE() << "unexpected exit status " << ws << " for descriptor " << dest << " sig " << sig;
            }
        }
    }

    // Test redirecting stdout but not stderr. Issue 40076.
    auto [er, ew, eerr] = os::Pipe();
    ASSERT_FALSE(eerr) << eerr;
    int ws = run_child(test_full_name(), {"GO_TEST_STD_PIPE_HELPER=1"}, -1, wfd, static_cast<int>(ew->Fd()), -1);
    ew->Close();
    std::string output = read_all(*er);
    er->Close();
    EXPECT_TRUE(WIFSIGNALED(ws) && WTERMSIG(ws) == SIGPIPE) << "unexpected exit status " << ws << " for write to closed stdout";
    // The child's test framework writes to stdout, which is the closed
    // pipe; it mustn't write to stderr. go++'s leak check reports there,
    // on exit, but the child dies before.
    EXPECT_EQ(output, "") << "unexpected output on stderr";
    w->Close();
}

namespace {

void test_closed_pipe_race(bool read) {
    // This test should give a full second to close the pipe, but a write
    // to a pipe whose reader isn't reading only blocks once the pipe is
    // full.
    std::size_t limit = 1;
    if (!read) {
        // Get the amount we have to write to overload a pipe with no
        // reader.
        limit = 131073;
        auto [data, err] = os::ReadFile("/proc/sys/fs/pipe-max-size");
        if (!err) limit = std::stoul(std::string(as_string_view(data))) + 1;
    }

    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    sync::WaitGroup wg;
    wg.Go([&] {
        // Give the main goroutine a chance to enter the Read or Write
        // call. This is sloppy but the test will pass even if we close
        // before the read/write.
        time::Sleep(20 * time::Millisecond);
        EXPECT_FALSE(read ? r->Close() : w->Close());
    });

    std::vector<byte> buf(limit);
    error ioerr = read ? r->Read(buf).second : w->Write(buf).second;
    if (!ioerr) {
        ADD_FAILURE() << "I/O on closed pipe unexpectedly succeeded";
    } else if (auto* pe = dynamic_cast<const fs::PathError*>(ioerr.get()); !pe) {
        ADD_FAILURE() << "I/O on closed pipe returned unexpected error " << ioerr << "; expected fs.PathError";
    } else if (pe->Err != fs::ErrClosed) {
        ADD_FAILURE() << "got error " << pe->Err << " but expected " << fs::ErrClosed;
    }
    wg.Wait();
    if (read) {
        w->Close();
    } else {
        r->Close();
    }
}

}  // namespace

TEST(GoOs, ClosedPipeRaceRead) { test_closed_pipe_race(true); }

TEST(GoOs, ClosedPipeRaceWrite) { test_closed_pipe_race(false); }

// Issue 20915: Reading on nonblocking fd should not return "waiting for
// unsupported file type." Currently it returns EAGAIN; it is possible that
// in the future it will simply wait for data.
TEST(GoOs, ReadNonblockingFd) {
    if (std::getenv("GO_WANT_READ_NONBLOCKING_FD")) {
        int fd = static_cast<int>(os::Stdin.Fd());
        int fl = ::fcntl(fd, F_GETFL);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        byte buf[1];
        auto [_, err] = os::Stdin.Read(buf);
        ::fcntl(fd, F_SETFL, fl);
        if (err) {
            auto* perr = dynamic_cast<const fs::PathError*>(err.get());
            if (!perr || !is_errno(perr->Err, EAGAIN)) {
                std::fprintf(stderr, "read on nonblocking stdin got %s, should have gotten EAGAIN\n", err.Error().view().data());
                os::Exit(1);
            }
        }
        os::Exit(0);
    }
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    int ws = run_child(test_full_name(), {"GO_WANT_READ_NONBLOCKING_FD=1"}, static_cast<int>(r->Fd()), -1, -1, -1);
    EXPECT_TRUE(WIFEXITED(ws) && WEXITSTATUS(ws) == 0) << "child process failed: " << ws;
    r->Close();
    w->Close();
}

namespace {

// Test that we don't let a blocking read prevent a close.
void test_close_with_blocking_read(const ptr<File>& r, const ptr<File>& w) {
    auto entering_read = make_chan<empty>();
    auto done = make_chan<empty>();
    go([&] {
        byte buf[1];
        entering_read.close();
        auto [_, err] = r->Read(buf);
        EXPECT_TRUE(err) << "I/O on closed pipe unexpectedly succeeded";
        if (auto* pe = dynamic_cast<const fs::PathError*>(err.get())) err = pe->Err;
        EXPECT_TRUE(err == io::EOF_ || err == fs::ErrClosed) << "got " << err << ", expected EOF or closed";
        done.close();
    });

    // Give the goroutine a chance to enter the Read or Write call. This is
    // sloppy but the test will pass even if we close before the
    // read/write.
    entering_read.recv();
    time::Sleep(20 * time::Millisecond);

    EXPECT_FALSE(r->Close());
    // r.Close has completed, but since we assume r is in blocking mode
    // that probably didn't unblock the call to r.Read. Close w to unblock
    // it.
    w->Close();
    done.recv();
}

}  // namespace

TEST(GoOs, CloseWithBlockingReadByNewFile) {
    int p[2];
    ASSERT_EQ(::pipe(p), 0);
    // os.NewFile returns a blocking mode file.
    test_close_with_blocking_read(os::NewFile(static_cast<uintptr>(p[0]), "reader"),
                                  os::NewFile(static_cast<uintptr>(p[1]), "writer"));
}

TEST(GoOs, CloseWithBlockingReadByFd) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    // Calling Fd will put the file into blocking mode.
    (void)r->Fd();
    test_close_with_blocking_read(r, w);
}

// Test that we don't get a spurious EOF when the read waits.
TEST(GoOs, PipeEOF) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    time::Duration park_delay = 10 * time::Millisecond;
    auto writer_done = make_chan<empty>();
    auto write = make_chan<int>(1);
    go([&] {
        for (;;) {
            auto [i, ok] = write.recv2();
            if (!ok) break;
            time::Sleep(park_delay);
            auto [_, werr] = fmt::Print(*w, "line {}\n", i);
            if (werr) {
                ADD_FAILURE() << "error writing to fifo: " << werr;
                writer_done.close();
                return;
            }
        }
        time::Sleep(park_delay);
        EXPECT_FALSE(w->Close()) << "error closing writer";
        writer_done.close();
    });

    bufio::Reader rbuf(r);
    for (int i = 0; i < 3; i++) {
        write.send(i);
        auto [line, rerr] = rbuf.ReadString('\n');
        ASSERT_FALSE(rerr) << rerr;
        EXPECT_EQ(line, "line " + std::to_string(i) + "\n");
    }
    write.close();
    auto [line, rerr] = rbuf.ReadString('\n');
    EXPECT_TRUE(rerr == io::EOF_ && line.empty()) << "ReadBytes: " << line << ", " << rerr << "; want \"\", io.EOF";
    EXPECT_FALSE(r->Close()) << "error closing reader";
    writer_done.recv();
}

// Issue 24481.
TEST(GoOs, FdRace) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    sync::WaitGroup wg;
    constexpr int tries = 100;
    for (int i = 0; i < tries; i++) {
        wg.Add(1);
        go([&] {
            w->Fd();
            wg.Done();
        });
    }
    wg.Wait();
    r->Close();
    w->Close();
}

TEST(GoOs, FdReadRace) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    constexpr int count = 10;
    auto c = make_chan<bool>(1);
    sync::WaitGroup wg;
    wg.Add(1);
    go([&] {
        byte buf[count];
        r->SetReadDeadline(time::Now().Add(time::Minute));
        c.send(true);
        auto [_, rerr] = r->Read(buf);
        EXPECT_FALSE(os::IsTimeout(rerr)) << "read timed out";
        wg.Done();
    });
    wg.Add(1);
    go([&] {
        c.recv();
        // Give the other goroutine a chance to enter the Read. It doesn't
        // matter if this occasionally fails, the test will still pass, it
        // just won't test anything.
        time::Sleep(10 * time::Millisecond);
        r->Fd();
        // The bug was that Fd would hang until Read timed out. If the bug
        // is fixed, then writing to w and closing r here will cause the
        // Read to exit before the timeout expires.
        byte zero[count] = {};
        w->Write(zero);
        r->Close();
        wg.Done();
    });
    wg.Wait();
    w->Close();
}

// os_unix_test.go

namespace {

void new_file_test(bool blocking) {
    int p[2];
    ASSERT_EQ(::pipe(p), 0) << "pipe";
    if (!blocking) {
        int fl = ::fcntl(p[0], F_GETFL);
        ASSERT_EQ(::fcntl(p[0], F_SETFL, fl | O_NONBLOCK), 0) << "SetNonblock";
    }
    // Set the read-side to non-blocking.
    ptr<File> file = os::NewFile(static_cast<uintptr>(p[0]), "notapipe");
    ASSERT_TRUE(file) << "failed to convert fd to file!";

    time::Duration time_to_write = 100 * time::Millisecond;
    time::Duration time_to_deadline = time::Millisecond;
    if (!blocking) time_to_write = time::Second;
    int wfd = p[1];
    auto timer = time::AfterFunc(time_to_write, [wfd] { (void)::write(wfd, "a", 1); });

    // Try to read with deadline (but don't block forever).
    byte buf[1];
    file->SetReadDeadline(time::Now().Add(time_to_deadline));
    auto [_, err] = file->Read(buf);
    if (!blocking) {
        // We want it to fail with a timeout.
        EXPECT_TRUE(errors::Is(err, os::ErrDeadlineExceeded)) << "No timeout reading from file: " << err;
    } else {
        // We want it to succeed after 100ms
        EXPECT_FALSE(err) << "Error reading from file: " << err;
    }
    timer->Stop();
    file->Close();
    ::close(p[1]);
}

}  // namespace

TEST(GoOs, NewFileBlock) { new_file_test(true); }

TEST(GoOs, NewFileNonBlock) { new_file_test(false); }

TEST(GoOs, NewFileInvalid) {
    const uintptr neg_one = ~uintptr(0);
    EXPECT_FALSE(os::NewFile(neg_one, "invalid")) << "NewFile(-1) got a File, want nil";
}
