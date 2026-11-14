// Ported from Go 1.27.3's os/exec tests: exec_test.go, exec_posix_test.go,
// lp_unix_test.go, dot_test.go and env_test.go, with Go's test names.
//
// Go's helper commands, which the test binary runs as when it's started
// with a command name, are helper commands of go_helper.hpp. Adapted:
// TestCatGoodAndBadFile cats a file of its own; TestExtraFiles reads a
// temporary file through descriptor 3 with a helper (Go builds read3.go)
// and checks the child for leaked descriptors through /proc/self/fd;
// TestExtraFilesRace passes temporary files instead of listeners (go++ has
// no net.FileListener); TestAbsPathExec runs /bin/true and /bin/false
// instead of gofmt and go. Not ported: TestWaitInterrupt/SIGQUIT (a Go
// goroutine dump), TestCredentialNoSetGroups (it works only as the user
// that ran it, which is all it checks; kept), TestFindExecutableVsNoexec
// (needs to mount a tmpfs), and the Windows tests.
#include "go_helper.hpp"
#include "go_os_test_util.hpp"
#include "os/exec/internal.hpp"

#include <gopp/bufio.hpp>
#include <gopp/context.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/os/signal.hpp>

#include <dirent.h>

#include <cstdlib>

using namespace os_test;
namespace exec = gopp::os::exec;
using go_helper::helper_command;
using go_helper::helperCommandContext;

namespace {

const std::string stdin_close_test_string = "Some test string.";

std::string joinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (std::size_t i = 0; i < args.size(); i++) s += (i ? " " : "") + args[i];
    return s;
}

// Go's ReadLine of bufio.Reader: a line without its end.
std::pair<std::string, error> read_line(bufio::Reader& br) {
    std::string line;
    for (;;) {
        byte b[1];
        auto [n, err] = br.Read(b);
        if (err) return {line, line.empty() ? err : nullptr};
        if (n == 0) continue;
        if (b[0] == '\n') return {line, nullptr};
        line.push_back(static_cast<char>(b[0]));
    }
}

// Go's helper commands.
void cmdEcho(const std::vector<std::string>& args) { fmt::Println("{}", joinArgs(args)); }

void cmdEchoEnv(const std::vector<std::string>& args) {
    for (auto& s : args) fmt::Println("{}", os::Getenv(s));
}

void cmdCat(const std::vector<std::string>& args) {
    if (args.empty()) {
        io::Copy(os::Stdout, os::Stdin);
        return;
    }
    int exit = 0;
    for (auto& fn : args) {
        auto [f, err] = os::Open(fn);
        if (err) {
            fmt::Println(os::Stderr, "Error: {}", err);
            exit = 2;
        } else {
            io::Copy(os::Stdout, *f);
            f->Close();
        }
    }
    os::Exit(exit);
}

void cmdPipeTest(const std::vector<std::string>&) {
    bufio::Reader bufr(ptr<io::Reader>(&os::Stdin));
    for (;;) {
        auto [line, err] = read_line(bufr);
        if (err == io::EOF_) break;
        if (err) os::Exit(1);
        if (line.starts_with("O:")) {
            os::Stdout.WriteString(line + "\n");
        } else if (line.starts_with("E:")) {
            os::Stderr.WriteString(line + "\n");
        } else {
            os::Exit(1);
        }
    }
}

void cmdStdinClose(const std::vector<std::string>&) {
    auto [b, err] = io::ReadAll(os::Stdin);
    if (err) {
        fmt::Println(os::Stderr, "Error: {}", err);
        os::Exit(1);
    }
    if (asStringView(b) != stdin_close_test_string) {
        fmt::Print(os::Stderr, "Error: Read {}, want {}", asStringView(b), stdin_close_test_string);
        os::Exit(1);
    }
}

void cmdExit(const std::vector<std::string>& args) { os::Exit(std::atoi(args.at(0).c_str())); }

// Go's describefiles, for temporary files: the contents of descriptor 3.
void cmdDescribeFiles(const std::vector<std::string>&) {
    auto f = os::NewFile(3, "fd3");
    std::string s = read_all(*f);
    fmt::Println("fd3: file {}", s);
}

void cmdStderrFail(const std::vector<std::string>&) {
    fmt::Print(os::Stderr, "some stderr text\n");
    os::Exit(1);
}

void cmdYes(const std::vector<std::string>& args) {
    std::string s = (args.empty() ? std::string("y") : joinArgs(args)) + "\n";
    for (;;) {
        if (os::Stdout.WriteString(s).second) os::Exit(1);
    }
}

void cmdPwd(const std::vector<std::string>&) {
    auto [pwd, err] = os::Getwd();
    if (err) {
        fmt::Println(os::Stderr, "{}", err);
        os::Exit(1);
    }
    fmt::Println("{}", pwd);
}

// Go's read3.go: copies descriptor 3 to standard output, and fails if the
// process inherited any other descriptor. The runtime's own (epoll,
// eventfd, io_uring) are anonymous inodes; the parent's pipes, files and
// sockets aren't.
void cmdRead3(const std::vector<std::string>&) {
    std::string leaked;
    if (DIR* d = ::opendir("/proc/self/fd")) {
        int self = ::dirfd(d);
        while (dirent* e = ::readdir(d)) {
            int fd = std::atoi(e->d_name);
            if (e->d_name[0] == '.' || fd <= 3 || fd == self) continue;
            char target[256] = {};
            std::string link = "/proc/self/fd/" + std::string(e->d_name);
            ssize_t n = ::readlink(link.c_str(), target, sizeof(target) - 1);
            if (n > 0 && std::string_view(target).starts_with("anon_inode:")) continue;
            leaked += " " + std::to_string(fd) + "=" + target;
        }
        ::closedir(d);
    }
    auto f = os::NewFile(3, "fd3");
    std::string s = read_all(*f);
    if (!leaked.empty()) {
        fmt::Println(os::Stderr, "leaked descriptors:{}", leaked);
        os::Exit(1);
    }
    fmt::Print("{}", s);
}

void cmd_hang(const std::vector<std::string>& args);

void cmdSignalTest(const std::vector<std::string>&);

const bool helpers_registered = [] {
    using go_helper::registerHelper;
    registerHelper("echo", cmdEcho);
    registerHelper("echoenv", cmdEchoEnv);
    registerHelper("cat", cmdCat);
    registerHelper("pipetest", cmdPipeTest);
    registerHelper("stdinClose", cmdStdinClose);
    registerHelper("exit", cmdExit);
    registerHelper("describefiles", cmdDescribeFiles);
    registerHelper("stderrfail", cmdStderrFail);
    registerHelper("yes", cmdYes);
    registerHelper("hang", cmd_hang);
    registerHelper("pwd", cmdPwd);
    registerHelper("signaltest", cmdSignalTest);
    registerHelper("read3", cmdRead3);
    return true;
}();

// Go's fmt.Errorf("%w: ...", err).
struct wrap_error final : errorInterface {
    wrap_error(error e, std::string msg) : err(std::move(e)), msg(std::move(msg)) {}
    string Error() const override { return err.Error() + ": " + string(msg); }
    const errorInterface* Unwrap() const noexcept override { return err.get(); }
    error err;
    std::string msg;
};

}  // namespace

TEST(GoOsExec, Echo) {
    auto [bs, err] = helper_command("echo", {"foo bar", "baz"})->Output();
    EXPECT_FALSE(err) << "echo: " << err;
    EXPECT_EQ(asStringView(bs), "foo bar baz\n");
}

TEST(GoOsExec, CommandRelativeName) {
    auto cmd = helper_command("echo", {"foo"});
    // Run our own binary as a relative path from its parent directory.
    std::string exe = str(go_helper::executable());
    std::string dir = exe.substr(0, exe.rfind('/'));
    std::string base = exe.substr(exe.rfind('/') + 1);
    if (dir.empty()) GTEST_SKIP() << "skipping; running test at root somehow";
    std::string parentDir = dir.substr(0, dir.rfind('/'));
    std::string dir_base = dir.substr(dir.rfind('/') + 1);
    if (parentDir.empty()) GTEST_SKIP() << "skipping; unexpected shallow dir of " << dir;

    cmd->Path = string(dir_base + "/" + base);
    cmd->Dir = string(parentDir);
    auto [out, err] = cmd->Output();
    EXPECT_FALSE(err) << "echo: " << err;
    EXPECT_EQ(asStringView(out), "foo\n");
}

TEST(GoOsExec, CatStdin) {
    // Cat, testing stdin and stdout.
    std::string input = "Input string\nLine 2";
    auto p = helper_command("cat");
    p->Stdin = strings::NewReader(input);
    auto [bs, err] = p->Output();
    EXPECT_FALSE(err) << "cat: " << err;
    EXPECT_EQ(asStringView(bs), input);
}

TEST(GoOsExec, EchoFileRace) {
    auto cmd = helper_command("echo");
    auto [stdin, err] = cmd->StdinPipe();
    ASSERT_FALSE(err) << "StdinPipe: " << err;
    ASSERT_FALSE(cmd->Start()) << "Start";
    auto wrote = makeChan<bool>();
    go([stdin, wrote] {
        fmt::Print(*stdin, "echo\n");
        wrote.close();
    });
    EXPECT_FALSE(cmd->Wait()) << "Wait";
    wrote.recv();
}

TEST(GoOsExec, CatGoodAndBadFile) {
    // Go cats exec_test.go; this cats a file of its own.
    NewFile f;
    f->WriteString("func TestCatGoodAndBadFile(t *testing.T)\n");
    // Testing combined output and error values.
    auto [bs, err] = helper_command("cat", {"/bogus/file.foo", str(f->Name())})->CombinedOutput();
    EXPECT_TRUE(errors::As<exec::ExitError>(err)) << "expected *exec.ExitError from cat combined; got " << err;
    auto [err_line, body, ok] = strings::Cut(String(bs), "\n");
    ASSERT_TRUE(ok) << "expected two lines from cat; got " << asStringView(bs);
    EXPECT_TRUE(err_line.starts_with("Error: open /bogus/file.foo")) << "expected stderr to complain about file; got "
                                                                     << err_line;
    EXPECT_TRUE(strings::Contains(body, "func TestCatGoodAndBadFile(t *testing.T)")) << "expected test code; got " << body;
}

TEST(GoOsExec, NoExistExecutable) {
    // Can't run a non-existent executable.
    EXPECT_TRUE(exec::Command("/no-exist-executable")->Run()) << "expected error from /no-exist-executable";
}

TEST(GoOsExec, ExitStatus) {
    // Test that exit values are returned correctly.
    auto cmd = helper_command("exit", {"42"});
    error err = cmd->Run();
    auto* werr = errors::As<exec::ExitError>(err);
    ASSERT_TRUE(werr) << "expected *exec.ExitError from exit 42; got " << err;
    EXPECT_EQ(err.Error(), "exit status 42");
    EXPECT_EQ(werr->ExitCode(), 42);
}

TEST(GoOsExec, ExitCode) {
    // Test that exit codes are returned correctly.
    auto cmd = helper_command("exit", {"42"});
    cmd->Run();
    EXPECT_EQ(cmd->ProcessState->ExitCode(), 42);

    cmd = helper_command("/no-exist-executable");
    cmd->Run();
    EXPECT_EQ(cmd->ProcessState->ExitCode(), 2);

    cmd = helper_command("exit", {"255"});
    cmd->Run();
    EXPECT_EQ(cmd->ProcessState->ExitCode(), 255);

    cmd = helper_command("cat");
    cmd->Run();
    EXPECT_EQ(cmd->ProcessState->ExitCode(), 0);

    // Test when command does not call Run(): Go's ExitCode of the nil
    // ProcessState is -1; here the ProcessState is nil.
    cmd = helper_command("cat");
    EXPECT_FALSE(cmd->ProcessState);
}

TEST(GoOsExec, Pipes) {
    // Cat, testing stdin and stdout.
    auto c = helper_command("pipetest");
    auto [stdin, err] = c->StdinPipe();
    ASSERT_FALSE(err) << "StdinPipe: " << err;
    auto [stdout, oerr] = c->StdoutPipe();
    ASSERT_FALSE(oerr) << "StdoutPipe: " << oerr;
    auto [stderr, eerr] = c->StderrPipe();
    ASSERT_FALSE(eerr) << "StderrPipe: " << eerr;

    bufio::Reader outbr(stdout), errbr(stderr);
    auto line = [](const char* what, bufio::Reader& br) {
        auto [line, err] = read_line(br);
        EXPECT_FALSE(err) << what << ": " << err;
        return line;
    };

    ASSERT_FALSE(c->Start()) << "Start";

    ASSERT_FALSE(stdin->Write(b("O:I am output\n")).second) << "first stdin Write";
    EXPECT_EQ(line("first output line", outbr), "O:I am output");

    ASSERT_FALSE(stdin->Write(b("E:I am error\n")).second) << "second stdin Write";
    EXPECT_EQ(line("first error line", errbr), "E:I am error");

    ASSERT_FALSE(stdin->Write(b("O:I am output2\n")).second) << "third stdin Write 3";
    EXPECT_EQ(line("second output line", outbr), "O:I am output2");

    stdin->Close();
    EXPECT_FALSE(c->Wait()) << "Wait";
}

// Go's issue 6270.
TEST(GoOsExec, StdinClose) {
    auto cmd = helper_command("stdinClose");
    auto [stdin, err] = cmd->StdinPipe();
    ASSERT_FALSE(err) << "StdinPipe: " << err;
    // The methods of the underlying os.File are there: StdinPipe returns it.
    EXPECT_GE(static_cast<intptr_t>(stdin->Fd()), 0);
    ASSERT_FALSE(cmd->Start()) << "Start";

    sync::WaitGroup wg;
    wg.Add(1);
    go([&] {
        auto [_, err] = io::Copy(*stdin, *strings::NewReader(stdin_close_test_string));
        EXPECT_FALSE(err) << "Copy: " << err;
        // Before Go's fix, this would race with cmd.Wait.
        if (error err = stdin->Close(); err && !errors::Is(err, os::ErrClosed)) ADD_FAILURE() << "Close: " << err;
        wg.Done();
    });
    EXPECT_FALSE(cmd->Wait()) << "Wait";
    wg.Wait();
}

// Go's issue 17647: a variant of TestStdinClose that used to fail under
// the race detector.
TEST(GoOsExec, StdinCloseRace) {
    auto cmd = helper_command("stdinClose");
    auto [stdin, err] = cmd->StdinPipe();
    ASSERT_FALSE(err) << "StdinPipe: " << err;
    ASSERT_FALSE(cmd->Start()) << "Start";

    sync::WaitGroup wg;
    wg.Add(2);
    go([&] {
        // The process may have exited already: Kill's error doesn't matter.
        cmd->Process->Kill();
        wg.Done();
    });
    go([&] {
        // Send the wrong string, so that the child fails even if the other
        // fiber doesn't manage to kill it first.
        io::Copy(*stdin, *strings::NewReader("unexpected string"));
        if (error err = stdin->Close(); err && !errors::Is(err, os::ErrClosed)) ADD_FAILURE() << "stdin.Close: " << err;
        wg.Done();
    });
    EXPECT_TRUE(cmd->Wait()) << "Wait: succeeded unexpectedly";
    wg.Wait();
}

namespace {

std::vector<int> openFds() {
    std::vector<int> fds;
    for (int i = 0; i < 100; i++) {
        if (::fcntl(i, F_GETFD) >= 0) fds.push_back(i);
    }
    return fds;
}

}  // namespace

// Go's issue 5071.
TEST(GoOsExec, PipeLookPathLeak) {
    std::vector<int> old = openFds();
    for (int i = 0; i < 6; i++) {
        auto cmd = exec::Command("something-that-does-not-exist-executable");
        cmd->StdoutPipe();
        cmd->StderrPipe();
        cmd->StdinPipe();
        ASSERT_TRUE(cmd->Run()) << "unexpected success";
    }
    for (int fd : openFds()) {
        EXPECT_NE(std::find(old.begin(), old.end(), fd), old.end()) << "leaked file descriptor " << fd;
    }
}

TEST(GoOsExec, ExtraFiles) {
    // Make sure that the epoll descriptor, sockets and pipes don't leak to
    // the child: use the network.
    auto [ln, lerr] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(lerr) << lerr;
    // Addr is a string here, a net::Addr on newer go++.
    auto addr = [](auto&& a) -> string {
        if constexpr (requires { a->String(); }) return a->String();
        else return a;
    }(ln->Addr());
    auto [conn, derr] = net::Dial("tcp", addr);
    ASSERT_FALSE(derr) << derr;
    auto [pr, pw, perr] = os::Pipe();
    ASSERT_FALSE(perr) << perr;

    auto [tf, err] = os::CreateTemp("", "");
    ASSERT_FALSE(err) << "TempFile: " << err;
    const std::string text = "Hello, fd 3!";
    ASSERT_FALSE(tf->WriteString(text).second);
    ASSERT_FALSE(tf->Seek(0, io::SeekStart).second);

    auto c = helper_command("read3");
    auto stdout = make_ptr<strings::Builder>(), stderr = make_ptr<strings::Builder>();
    c->Stdout = stdout;
    c->Stderr = stderr;
    c->ExtraFiles = {tf};
    err = c->Run();
    EXPECT_FALSE(err) << "Run: " << err << "\n--- stdout:\n" << stdout->view() << "--- stderr:\n" << stderr->view();
    EXPECT_EQ(stdout->view(), text) << "stderr " << stderr->view();

    tf->Close();
    os::Remove(tf->Name());
    conn->Close();
    ln->Close();
    pr->Close();
    pw->Close();
}

TEST(GoOsExec, ExtraFilesRace) {
    // Go passes listeners; these are temporary files with contents of
    // their own: each child must see its own as descriptor 3.
    auto temp_file = [](const std::string& content) {
        auto [f, err] = os::CreateTemp("", "extrafiles");
        EXPECT_FALSE(err) << err;
        f->WriteString(content);
        f->Seek(0, io::SeekStart);
        return f;
    };
    auto run_command = [](ptr<exec::Cmd> c, chan<std::string> out) {
        auto [bout, err] = c->CombinedOutput();
        if (err) out.send("ERROR:" + str(err.Error()));
        else out.send(std::string(asStringView(bout)));
    };
    for (int i = 0; i < 10; i++) {
        std::string a = "A" + std::to_string(i), b_ = "B" + std::to_string(i);
        auto fa = temp_file(a), fb = temp_file(b_);
        auto ca = helper_command("describefiles");
        ca->ExtraFiles = {fa};
        auto cb = helper_command("describefiles");
        cb->ExtraFiles = {fb};
        auto ares = makeChan<std::string>(), bres = makeChan<std::string>();
        go(run_command, ca, ares);
        go(run_command, cb, bres);
        EXPECT_EQ(ares.recv(), "fd3: file " + a + "\n") << "iteration " << i << ", process A";
        EXPECT_EQ(bres.recv(), "fd3: file " + b_ + "\n") << "iteration " << i << ", process B";
        for (auto& f : {fa, fb}) {
            f->Close();
            os::Remove(f->Name());
        }
    }
}

namespace {

// Go's delayedInfiniteReader.
struct delayed_infinite_reader final : io::Reader {
    std::pair<isize, error> Read(std::span<byte> p) override {
        time::Sleep(100 * time::Millisecond);
        std::fill(p.begin(), p.end(), 'x');
        return {static_cast<isize>(p.size()), nullptr};
    }
};

struct bad_writer final : io::Writer {
    std::pair<isize, error> Write(std::span<const byte>) override { return {0, io::ErrUnexpectedEOF}; }
};

}  // namespace

// Go's issue 9173: ignore stdin pipe writes if the program completes
// successfully.
TEST(GoOsExec, IgnorePipeErrorOnSuccess) {
    auto test_with = [](ptr<io::Reader> r) {
        auto cmd = helper_command("echo", {"foo"});
        auto out = make_ptr<strings::Builder>();
        cmd->Stdin = r;
        cmd->Stdout = out;
        EXPECT_FALSE(cmd->Run());
        EXPECT_EQ(out->view(), "foo\n");
    };
    {
        SCOPED_TRACE("10MB");
        test_with(strings::NewReader(strings::Repeat("x", 10 << 20)));
    }
    {
        SCOPED_TRACE("Infinite");
        test_with(make_ptr<delayed_infinite_reader>());
    }
}

TEST(GoOsExec, ClosePipeOnCopyError) {
    auto cmd = helper_command("yes");
    cmd->Stdout = make_ptr<bad_writer>();
    EXPECT_TRUE(cmd->Run()) << "yes unexpectedly completed successfully";
}

TEST(GoOsExec, OutputStderrCapture) {
    auto cmd = helper_command("stderrfail");
    auto [_, err] = cmd->Output();
    auto* ee = errors::As<exec::ExitError>(err);
    ASSERT_TRUE(ee) << "Output error = " << err << "; want ExitError";
    EXPECT_EQ(asStringView(ee->Stderr), "some stderr text\n");
}

TEST(GoOsExec, Context) {
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto c = helperCommandContext(ctx, "pipetest");
    auto [stdin, err] = c->StdinPipe();
    ASSERT_FALSE(err) << err;
    auto [stdout, oerr] = c->StdoutPipe();
    ASSERT_FALSE(oerr) << oerr;
    ASSERT_FALSE(c->Start());

    ASSERT_FALSE(stdin->Write(b("O:hi\n")).second);
    byte buf[5];
    auto [n, rerr] = io::ReadFull(*stdout, buf);
    ASSERT_TRUE(n == 5 && !rerr && asStringView(std::span<const byte>(buf, 5)) == "O:hi\n")
        << "ReadFull = " << n << ", " << rerr;
    go(cancel);
    EXPECT_TRUE(c->Wait()) << "expected Wait failure";
}

TEST(GoOsExec, ContextCancel) {
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto c = helperCommandContext(ctx, "cat");
    auto [stdin, err] = c->StdinPipe();
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(c->Start());

    // The process is alive: send data to stdin.
    ASSERT_FALSE(io::WriteString(*stdin, "echo").second);

    cancel();

    // Canceling killed the process, so writes now fail. Give the process
    // a little while to die.
    auto start = time::Now();
    time::Duration delay = time::Millisecond;
    for (;;) {
        if (io::WriteString(*stdin, "echo").second) break;
        ASSERT_LT(time::Since(start), time::Minute) << "canceling context did not stop program";
        // Back off exponentially (up to 1-second sleeps).
        delay = delay * 2;
        if (delay > time::Second) delay = time::Second;
        time::Sleep(delay);
    }
    EXPECT_TRUE(c->Wait()) << "program unexpectedly exited successfully";
    stdin->Close();
}

// Test that environment variables are de-duped.
TEST(GoOsExec, DedupEnvEcho) {
    auto cmd = helper_command("echoenv", {"FOO"});
    cmd->Env = append(cmd->Environ(), string("FOO=bad"), string("FOO=good"));
    auto [out, err] = cmd->CombinedOutput();
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(strings::TrimSpace(String(out)), "good");
}

TEST(GoOsExec, EnvNULCharacter) {
    auto cmd = helper_command("echoenv", {"FOO", "BAR"});
    cmd->Env = append(cmd->Environ(), string(std::string_view("FOO=foo\0BAR=bar", 15)));
    auto [out, err] = cmd->CombinedOutput();
    EXPECT_TRUE(err) << "output = " << asStringView(out) << "; want error";
}

TEST(GoOsExec, String) {
    auto [echo_path, err] = exec::LookPath("echo");
    if (err) GTEST_SKIP() << err;
    struct test {
        std::string path;
        std::vector<string> args;
        std::string want;
    };
    std::string ep = str(echo_path);
    for (const test& t : std::vector<test>{
             {"echo", {}, ep},
             {"echo", {"a"}, ep + " a"},
             {"echo", {"a", "b"}, ep + " a b"},
         }) {
        auto cmd = exec::Command(t.path, t.args);
        EXPECT_EQ(cmd->String(), t.want);
    }
}

TEST(GoOsExec, StringPathNotResolved) {
    if (!exec::LookPath("makemeasandwich").second) GTEST_SKIP() << "wow, thanks";
    auto cmd = exec::Command("makemeasandwich", "-lettuce");
    EXPECT_EQ(cmd->String(), "makemeasandwich -lettuce");
}

TEST(GoOsExec, NoPath) {
    error err = make_ptr<exec::Cmd>()->Start();
    EXPECT_TRUE(err && err.Error() == "exec: no command") << "new(Cmd).Start() = " << err;
}

// Calling Start twice, which fails the second time, mustn't close the
// pipes of the first.
TEST(GoOsExec, DoubleStartLeavesPipesOpen) {
    auto cmd = helper_command("pipetest");
    auto [in, err] = cmd->StdinPipe();
    ASSERT_FALSE(err) << err;
    auto [out, oerr] = cmd->StdoutPipe();
    ASSERT_FALSE(oerr) << oerr;
    ASSERT_FALSE(cmd->Start());

    error err2 = cmd->Start();
    EXPECT_TRUE(err2 && strings::HasSuffix(err2.Error(), "already started"))
        << "second call to Start returned a nil; want an 'already started' error";

    auto outc = makeChan<std::string>(1);
    go([out, outc] {
        auto [b, err] = io::ReadAll(*out);
        EXPECT_FALSE(err) << err;
        outc.send(std::string(asStringView(b)));
    });
    const std::string msg = "O:Hello, pipe!\n";
    ASSERT_FALSE(io::WriteString(*in, msg).second);
    in->Close();
    EXPECT_EQ(outc.recv(), msg);
    EXPECT_FALSE(cmd->Wait());
}

namespace {

// Go's cmdHang: sleeps for args[0], with flags -interrupt, -subsleep,
// -probe and -read.
void cmd_hang(const std::vector<std::string>& args) {
    auto [sleep, err] = time::ParseDuration(args.at(0));
    if (err) panic("bad duration");
    bool exit_on_interrupt = false, read = false;
    time::Duration subsleep, probe;
    for (std::size_t i = 1; i < args.size(); i++) {
        std::string_view a = args[i];
        auto [k, v, ok] = strings::Cut(string(a), "=");
        if (k == "-interrupt") exit_on_interrupt = v == "true";
        else if (k == "-read") read = v == "true";
        else if (k == "-subsleep") subsleep = time::ParseDuration(v).first;
        else if (k == "-probe") probe = time::ParseDuration(v).first;
    }
    int pid = os::Getpid();

    if (subsleep.ns != 0) {
        auto cmd = exec::Command(go_helper::executable(), "hang", subsleep.String(), "-read=true",
                                 "-probe=" + probe.String().str());
        cmd->Stdin = ptr<io::Reader>(&os::Stdin);
        cmd->Stderr = ptr<io::Writer>(&os::Stderr);
        auto [out, err] = cmd->StdoutPipe();
        if (err) {
            fmt::Println(os::Stderr, "{}", err);
            os::Exit(1);
        }
        cmd->Start();
        strings::Builder buf;
        if (auto [_, cerr] = io::Copy(buf, *out); cerr) {
            fmt::Println(os::Stderr, "{}", cerr);
            cmd->Process->Kill();
            cmd->Wait();
            os::Exit(1);
        }
        fmt::Print(os::Stderr, "{}: started {}: {}\n", pid, cmd->Process->Pid, cmd->String());
        go([cmd] { cmd->Wait(); });  // release resources if cmd doesn't outlive this process
    }

    if (exit_on_interrupt) {
        auto c = makeChan<os::Signal>(1);
        os::signal::Notify(c, os::Interrupt);
        go([c, pid] {
            os::Signal sig = c.recv();
            fmt::Print(os::Stderr, "{}: received {}\n", pid, sig);
            os::Exit(0);
        });
    } else {
        os::signal::Ignore(os::Interrupt);
    }

    // Tell the parent that the process is set up: close stdout.
    os::Stdout.Close();

    if (read) {
        os::signal::Ignore(syscall::SIGPIPE_);
        bufio::Reader r(ptr<io::Reader>(&os::Stdin));
        for (;;) {
            auto [line, err] = read_line(r);
            // Ignore write errors: keep reading even if stderr is closed.
            if (!line.empty()) fmt::Print(os::Stderr, "{}: read {}\n", pid, line);
            if (err) {
                fmt::Print(os::Stderr, "{}: finished read: {}", pid, err);
                break;
            }
        }
    }

    if (probe.ns != 0) {
        auto ticker = time::NewTicker(probe);
        go([ticker, pid] {
            for (;;) {
                ticker->C.recv();
                if (fmt::Print(os::Stderr, "{}: ok\n", pid).second) os::Exit(1);
            }
        });
    }

    if (sleep.ns != 0) {
        time::Sleep(sleep);
        fmt::Print(os::Stderr, "{}: slept {}\n", pid, sleep);
    }
}

// Go's tickReader: timestamps at no more than a fixed interval.
class tick_reader final : public io::Reader {
public:
    explicit tick_reader(time::Duration interval) : interval_(interval) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (s_.empty()) {
            time::Duration d = interval_ - time::Since(last_tick_);
            if (d.ns > 0) time::Sleep(d);
            last_tick_ = time::Now();
            s_ = std::to_string(last_tick_.UnixNano()) + "\n";
        }
        std::size_t n = std::min(p.size(), s_.size());
        std::copy_n(s_.begin(), n, p.begin());
        s_.erase(0, n);
        return {static_cast<isize>(n), nullptr};
    }

private:
    time::Duration interval_;
    time::Time last_tick_;
    std::string s_;
};

// Go's startHang.
ptr<exec::Cmd> start_hang(ptr<context::Context> ctx, time::Duration hang_time, os::Signal interrupt,
                          time::Duration wait_delay, std::vector<std::string> flags = {},
                          ptr<strings::Builder>* stderr_out = nullptr) {
    std::vector<std::string> args{str(hang_time.String())};
    args.insert(args.end(), flags.begin(), flags.end());
    auto cmd = helperCommandContext(ctx, "hang", args);
    cmd->Stdin = make_ptr<tick_reader>(time::Millisecond);
    auto stderr = make_ptr<strings::Builder>();
    cmd->Stderr = stderr;
    if (stderr_out) *stderr_out = stderr;
    if (!interrupt) {
        cmd->Cancel = nullptr;
    } else {
        exec::Cmd* c = cmd.get();
        cmd->Cancel = [c, interrupt] { return c->Process->Signal(interrupt); };
    }
    cmd->WaitDelay = wait_delay;
    auto [out, err] = cmd->StdoutPipe();
    EXPECT_FALSE(err) << err;
    EXPECT_FALSE(cmd->Start());

    // Wait for cmd to close stdout to signal that its handlers are installed.
    strings::Builder buf;
    if (auto [_, cerr] = io::Copy(buf, *out); cerr) {
        ADD_FAILURE() << cerr;
        cmd->Process->Kill();
        cmd->Wait();
    }
    return cmd;
}

}  // namespace

TEST(GoOsExec, WaitInterrupt) {
    // Much longer than the test runs, but short enough that leaked
    // processes eventually exit on their own.
    const time::Duration too_long = 10 * time::Minute;

    {
        // Control case: with no cancellation and no WaitDelay, we wait for
        // the process to exit.
        SCOPED_TRACE("Wait");
        auto cmd = start_hang(context::Background(), time::Millisecond, os::Kill, time::Duration());
        error err = cmd->Wait();
        EXPECT_FALSE(err) << "Wait: " << err << "; want <nil>";
        auto ps = cmd->ProcessState;
        EXPECT_TRUE(ps->Exited()) << "cmd did not exit: " << ps->String();
        EXPECT_EQ(ps->ExitCode(), 0);
    }
    {
        // With a very long WaitDelay and no Cancel function, we wait for the
        // process to exit even if the command's Context is canceled.
        SCOPED_TRACE("WaitDelay");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = start_hang(ctx, too_long, nullptr, too_long, {"-interrupt=true"});
        cancel();
        time::Sleep(time::Millisecond);
        // cmd is still running: an explicit Interrupt succeeds.
        EXPECT_FALSE(cmd->Process->Signal(os::Interrupt));
        error err = cmd->Wait();
        // The program exits with status 0, during the wait delay: a late
        // but successful exit merits no error.
        EXPECT_FALSE(err) << "Wait: " << err << "; want nil";
        EXPECT_TRUE(cmd->ProcessState->Exited());
        EXPECT_EQ(cmd->ProcessState->ExitCode(), 0);
    }
    {
        // If the context is canceled and Cancel sends os.Kill, the process is
        // terminated at once, and its output pipes are closed (Wait returns)
        // after WaitDelay even if a child process still writes to them.
        SCOPED_TRACE("SIGKILL-hang");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = start_hang(ctx, too_long, os::Kill, 10 * time::Millisecond, {"-subsleep=10m", "-probe=1ms"});
        cancel();
        error err = cmd->Wait();
        EXPECT_TRUE(errors::As<exec::ExitError>(err)) << "Wait error = " << err << "; want *exec.ExitError";
    }
    {
        // If the process exits with status 0 but leaves a child writing to
        // its output pipes, Wait waits only WaitDelay before closing the
        // pipes, and returns ErrWaitDelay.
        SCOPED_TRACE("Exit-hang");
        auto cmd = start_hang(context::Background(), time::Millisecond, nullptr, 10 * time::Millisecond,
                              {"-subsleep=10m", "-probe=1ms"});
        error err = cmd->Wait();
        EXPECT_TRUE(errors::Is(err, exec::ErrWaitDelay)) << "Wait error = " << err << "; want ErrWaitDelay";
    }
    {
        // If Cancel sends a signal the process handles without exiting, it's
        // terminated after WaitDelay.
        SCOPED_TRACE("SIGINT-ignored");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = start_hang(ctx, too_long, os::Interrupt, 10 * time::Millisecond, {"-interrupt=false"});
        cancel();
        error err = cmd->Wait();
        EXPECT_TRUE(errors::As<exec::ExitError>(err)) << "Wait error = " << err << "; want *exec.ExitError";
    }
    {
        // If the process handles the cancellation signal and exits with
        // status 0, Wait reports the context's error.
        SCOPED_TRACE("SIGINT-handled");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = start_hang(ctx, too_long, os::Interrupt, time::Duration(), {"-interrupt=true"});
        cancel();
        error err = cmd->Wait();
        EXPECT_TRUE(errors::Is(err, ctx->Err())) << "Wait error = " << err << "; want " << ctx->Err();
        EXPECT_TRUE(cmd->ProcessState->Exited());
        EXPECT_EQ(cmd->ProcessState->ExitCode(), 0);
    }
    // SIGQUIT: Go checks for a goroutine dump.
}

TEST(GoOsExec, CancelErrors) {
    {
        // If Cancel returns a non-ErrProcessDone error and the process exits
        // successfully, Wait wraps the error from Cancel.
        SCOPED_TRACE("success after error");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = helperCommandContext(ctx, "pipetest");
        auto [stdin, err] = cmd->StdinPipe();
        ASSERT_FALSE(err);
        error err_arbitrary = errors::New("arbitrary error");
        cmd->Cancel = [stdin, err_arbitrary] {
            stdin->Close();
            return err_arbitrary;
        };
        ASSERT_FALSE(cmd->Start());
        cancel();
        error werr = cmd->Wait();
        EXPECT_TRUE(errors::Is(werr, err_arbitrary) && werr != err_arbitrary)
            << "Wait error = " << werr << "; want an error wrapping " << err_arbitrary;
    }
    {
        // If Cancel returns an error equivalent to ErrProcessDone, Wait
        // ignores it.
        SCOPED_TRACE("success after ErrProcessDone");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = helperCommandContext(ctx, "pipetest");
        auto [stdin, err] = cmd->StdinPipe();
        ASSERT_FALSE(err);
        auto [stdout, oerr] = cmd->StdoutPipe();
        ASSERT_FALSE(oerr);
        // Race Cancel against the process exiting, but let the process win.
        auto interrupt_called = makeChan<empty>(), done = makeChan<empty>();
        cmd->Cancel = [interrupt_called, done]() -> error {
            interrupt_called.close();
            done.recv();
            return make_ptr<wrap_error>(os::ErrProcessDone, "stdout closed");
        };
        ASSERT_FALSE(cmd->Start());
        cancel();
        interrupt_called.recv();
        stdin->Close();
        io::Copy(io::Discard(), *stdout);  // EOF when the process exits
        done.close();
        error werr = cmd->Wait();
        EXPECT_FALSE(werr) << "Wait error = " << werr << "; want nil";
    }
    {
        // If Cancel returns an error and the process is killed after
        // WaitDelay, Wait reports the usual ExitError of SIGKILL.
        SCOPED_TRACE("killed after error");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = helperCommandContext(ctx, "pipetest");
        auto [stdin, err] = cmd->StdinPipe();
        ASSERT_FALSE(err);
        error err_arbitrary = errors::New("arbitrary error");
        auto interrupt_called = std::make_shared<std::atomic<bool>>(false);
        cmd->Cancel = [interrupt_called, err_arbitrary] {
            interrupt_called->store(true);
            return err_arbitrary;
        };
        cmd->WaitDelay = time::Millisecond;
        ASSERT_FALSE(cmd->Start());
        cancel();
        error werr = cmd->Wait();
        EXPECT_TRUE(interrupt_called->load()) << "Cancel was not called when the context was canceled";
        // The error is an ExitError without wrapping.
        EXPECT_TRUE(dynamic_cast<const exec::ExitError*>(werr.get())) << "Wait error = " << werr << "; want *exec.ExitError";
        stdin->Close();
    }
    {
        // If Cancel returns ErrProcessDone but the process isn't done (and has
        // to be killed), Wait reports the usual ExitError.
        SCOPED_TRACE("killed after spurious ErrProcessDone");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = helperCommandContext(ctx, "pipetest");
        auto [stdin, err] = cmd->StdinPipe();
        ASSERT_FALSE(err);
        auto interrupt_called = std::make_shared<std::atomic<bool>>(false);
        cmd->Cancel = [interrupt_called]() -> error {
            interrupt_called->store(true);
            return make_ptr<wrap_error>(os::ErrProcessDone, "stdout closed");
        };
        cmd->WaitDelay = time::Millisecond;
        ASSERT_FALSE(cmd->Start());
        cancel();
        error werr = cmd->Wait();
        EXPECT_TRUE(interrupt_called->load()) << "Cancel was not called when the context was canceled";
        EXPECT_TRUE(dynamic_cast<const exec::ExitError*>(werr.get())) << "Wait error = " << werr << "; want *exec.ExitError";
        stdin->Close();
    }
    {
        // If Cancel returns an error and the process exits unsuccessfully,
        // the process's error takes precedence.
        SCOPED_TRACE("nonzero exit after error");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto cmd = helperCommandContext(ctx, "stderrfail");
        auto [stderr, err] = cmd->StderrPipe();
        ASSERT_FALSE(err);
        error err_arbitrary = errors::New("arbitrary error");
        auto interrupted = makeChan<empty>();
        cmd->Cancel = [interrupted, err_arbitrary] {
            interrupted.close();
            return err_arbitrary;
        };
        ASSERT_FALSE(cmd->Start());
        cancel();
        interrupted.recv();
        io::Copy(io::Discard(), *stderr);
        error werr = cmd->Wait();
        auto* ee = dynamic_cast<const exec::ExitError*>(werr.get());
        EXPECT_TRUE(ee && ee->ProcessState->ExitCode() == 1) << "Wait error = " << werr << "; want exit status 1";
    }
}

// Go's issue 61080: starting many processes at once.
TEST(GoOsExec, ConcurrentExec) {
    auto [ctx, cancel] = context::WithCancel(context::Background());
    const int n_hangs = runtime::GOMAXPROCS(), n_exits = runtime::GOMAXPROCS();
    sync::WaitGroup hangs, exits, ready;
    hangs.Add(n_hangs);
    exits.Add(n_exits);
    ready.Add(n_hangs + n_exits);

    for (int i = 0; i < n_hangs; i++) {
        go([&, ctx] {
            auto cmd = helperCommandContext(ctx, "pipetest");
            auto [stdin, err] = cmd->StdinPipe();
            if (err) {
                ready.Done();
                ADD_FAILURE() << err;
                hangs.Done();
                return;
            }
            cmd->Cancel = [stdin] { return stdin->Close(); };
            ready.Done();
            ready.Wait();
            if (error err = cmd->Start()) {
                if (!errors::Is(err, context::Canceled)) ADD_FAILURE() << err;
            } else {
                cmd->Wait();
            }
            hangs.Done();
        });
    }
    for (int i = 0; i < n_exits; i++) {
        go([&, ctx] {
            auto cmd = helperCommandContext(ctx, "exit", {"0"});
            ready.Done();
            ready.Wait();
            if (error err = cmd->Run()) ADD_FAILURE() << err;
            exits.Done();
        });
    }
    exits.Wait();
    cancel();
    hangs.Wait();
}

// Cmd::String can be called while Start runs.
TEST(GoOsExec, PathRace) {
    auto cmd = helper_command("exit", {"0"});
    auto done = makeChan<empty>();
    go([cmd, done] {
        cmd->CombinedOutput();
        done.close();
    });
    EXPECT_FALSE(cmd->String().empty());
    done.recv();
}

TEST(GoOsExec, AbsPathExec) {
    // A simple exec of a full path works. (Go runs gofmt.)
    const std::string exe = "/bin/true";
    auto cmd = exec::Command(exe);
    EXPECT_EQ(cmd->Path, exe);
    EXPECT_FALSE(cmd->Run()) << "using exec.Command(" << exe << ")";

    cmd = make_ptr<exec::Cmd>();
    cmd->Path = string(exe);
    EXPECT_FALSE(cmd->Run()) << "using exec.Cmd{Path: " << exe << "}";

    cmd = make_ptr<exec::Cmd>();
    cmd->Path = "true";
    cmd->Dir = "/";
    EXPECT_TRUE(cmd->Run()) << "using exec.Cmd{Path: true}: unexpected success";

    // A simple exec after modifying Cmd.Path works (Go's issue 68314).
    ASSERT_TRUE(exec::Command("/bin/false")->Run());
    cmd = exec::Command("/bin/false");
    cmd->Path = string(exe);
    cmd->Args = slice<string>{cmd->Path};
    EXPECT_FALSE(cmd->Run()) << "ran wrong binary";
}

// Calling Start twice is an error, regardless of outcome.
TEST(GoOsExec, Start_twice) {
    auto cmd = exec::Command("/bin/nonesuch");
    ASSERT_TRUE(cmd->Start()) << "running invalid command succeeded";
    error err = cmd->Start();
    EXPECT_EQ(err.Error(), "exec: already started");
}

TEST(GoOsExec, NoStringPanic) {
    // Go formats a Cmd{} with %v.
    exec::Cmd c;
    EXPECT_EQ(c.String(), "");
}

// exec_posix_test.go

TEST(GoOsExec, CredentialNoSetGroups) {
    // With NoSetGroups, setgroups isn't called, and running as ourselves
    // succeeds.
    auto cmd = helper_command("echo", {"foo"});
    syscall::SysProcAttr sys;
    sys.Credential = syscall::Credential{.Uid = static_cast<uint32>(os::Getuid()),
                                         .Gid = static_cast<uint32>(os::Getgid()),
                                         .NoSetGroups = true};
    cmd->SysProcAttr = sys;
    EXPECT_FALSE(cmd->Run()) << "Failed to run command";
}

TEST(GoOsExec, Waitid) {
    auto cmd = helper_command("pipetest");
    auto [stdin, err] = cmd->StdinPipe();
    ASSERT_FALSE(err);
    auto [stdout, oerr] = cmd->StdoutPipe();
    ASSERT_FALSE(oerr);
    ASSERT_FALSE(cmd->Start());

    // Wait for the child to come up.
    const std::string msg = "O:ping\n";
    ASSERT_FALSE(io::WriteString(*stdin, msg).second);
    std::vector<byte> buf(msg.size());
    ASSERT_FALSE(io::ReadFull(*stdout, buf).second);
    // Leave the pipes open: the process hangs until stdin is closed.

    if (error err = cmd->Process->Signal(syscall::SIGSTOP_)) {
        cmd->Process->Kill();
        FAIL() << err;
    }
    auto ch = makeChan<error>();
    go([cmd, ch] { ch.send(cmd->Wait()); });

    // Give Wait a little time to block.
    time::Sleep(10 * time::Millisecond);

    // The process still exists: Signal succeeds. (Before Go's fix for
    // issue 19314 this failed with ErrProcessDone.)
    if (error err = cmd->Process->Signal(syscall::SIGCONT_)) {
        ADD_FAILURE() << err;
        syscall::Kill(cmd->Process->Pid, syscall::SIGCONT_);
    }
    // SIGCONT wakes the process, which sees stdin closed and exits.
    stdin->Close();
    EXPECT_FALSE(ch.recv());
}

namespace {

std::string parentDir(const std::string& d) {
    std::size_t i = d.rfind('/');
    return i == 0 ? "/" : d.substr(0, i);
}

}  // namespace

TEST(GoOsExec, ImplicitPWD) {
    auto [cwd_s, err] = os::Getwd();
    ASSERT_FALSE(err) << err;
    std::string cwd = str(cwd_s);
    struct tc {
        std::string name, dir, want;
    };
    for (const tc& c : std::vector<tc>{
             {"empty", "", cwd},
             {"dot", ".", cwd},
             {"dotdot", "..", parentDir(cwd)},
             {"PWD", cwd, cwd},
             {"PWDdotdot", cwd + "/..", parentDir(cwd)},
         }) {
        SCOPED_TRACE(c.name);
        auto cmd = helper_command("pwd");
        ASSERT_TRUE(cmd->Env.isNil()) << "test requires helperCommand not to set Env field";
        cmd->Dir = string(c.dir);

        std::vector<std::string> pwds;
        for (const string& kv : cmd->Environ()) {
            if (kv.starts_with("PWD=")) pwds.emplace_back(kv.view().substr(4));
        }
        std::vector<std::string> want_pwds{c.want};
        if (c.dir.empty() && !os::LookupEnv("PWD").second) want_pwds.clear();
        EXPECT_EQ(pwds, want_pwds);

        auto stderr = make_ptr<strings::Builder>();
        cmd->Stderr = stderr;
        auto [out, oerr] = cmd->Output();
        ASSERT_FALSE(oerr) << oerr << ":\n" << stderr->view();
        EXPECT_EQ(strings::Trim(String(out), "\r\n"), c.want);
    }
}

TEST(GoOsExec, ExplicitPWD) {
    auto [cwd_s, err] = os::Getwd();
    ASSERT_FALSE(err) << err;
    std::string cwd = str(cwd_s);
    TempDir tmp;
    std::string link = join(tmp.path(), "link");
    ASSERT_FALSE(os::Symlink(cwd, link));

    // link is another name for cwd. With Dir one and PWD the other, the
    // subprocess reports the PWD one.
    struct tc {
        std::string name, dir, pwd;
    };
    for (const tc& c : std::vector<tc>{
             {"original PWD", "", cwd},
             {"link PWD", "", link},
             {"in link with original PWD", link, cwd},
             {"in dir with link PWD", cwd, link},
         }) {
        SCOPED_TRACE(c.name);
        auto cmd = helper_command("pwd");
        // PWD is set before Dir: it's meant not to match Dir.
        cmd->Env = append(cmd->Environ(), string("PWD=" + c.pwd));
        cmd->Dir = string(c.dir);

        std::vector<std::string> pwds;
        for (const string& kv : cmd->Environ()) {
            if (kv.starts_with("PWD=")) pwds.emplace_back(kv.view().substr(4));
        }
        EXPECT_EQ(pwds, std::vector<std::string>{c.pwd});

        auto stderr = make_ptr<strings::Builder>();
        cmd->Stderr = stderr;
        auto [out, oerr] = cmd->Output();
        ASSERT_FALSE(oerr) << oerr << ":\n" << stderr->view();
        EXPECT_EQ(strings::Trim(String(out), "\r\n"), c.pwd);
    }
}

namespace {

void cmdSignalTest(const std::vector<std::string>&) {
    auto ch_sig = makeChan<os::Signal>(1);
    os::signal::Notify(ch_sig, syscall::SIGCHLD_);

    sync::WaitGroup wg;
    wg.Add(1);
    go([&] {
        int c = 0;
        for (auto s : ch_sig) {
            (void)s;
            c++;
            fmt::Println("SIGCHLD {}", c);
            if (c > 1) {
                fmt::Println("too many SIGCHLD signals");
                os::Exit(1);
            }
        }
        wg.Done();
    });

    auto cmd = exec::Command(go_helper::executable(), "hang", "200ms");
    cmd->Stdout = ptr<io::Writer>(&os::Stdout);
    cmd->Stderr = ptr<io::Writer>(&os::Stderr);
    error err = cmd->Run();
    os::signal::Reset(syscall::SIGCHLD_);
    ch_sig.close();
    wg.Wait();
    if (err) {
        fmt::Println("failed to run child process: {}", err);
        os::Exit(1);
    }
}

}  // namespace

// Go's TestSIGCHLD (SIGCHLD is a C macro).
TEST(GoOsExec, SIGCHLD_) {
    auto [out, err] = helper_command("signaltest")->CombinedOutput();
    EXPECT_FALSE(err) << err << "\n" << asStringView(out);
}

// lp_unix_test.go

TEST(GoOsExec, LookPathUnixEmptyPath) {
    InTempDir tmp;
    auto [f, err] = os::OpenFile("exec_me", os::O_CREATE_ | os::O_EXCL_, 0700);
    ASSERT_FALSE(err) << "OpenFile failed: " << err;
    ASSERT_FALSE(f->Close());

    string old = os::Getenv("PATH");
    os::Setenv("PATH", "");
    auto [path, lerr] = exec::LookPath("exec_me");
    os::Setenv("PATH", old);
    EXPECT_TRUE(lerr) << "LookPath found exec_me in empty $PATH";
    EXPECT_EQ(path, "") << "LookPath path when err != nil";
}

// dot_test.go

namespace {

class setenvGuard {
public:
    setenvGuard(const std::string& k, const std::string& v) : k_(k) {
        std::tie(old_, had_) = os::LookupEnv(k);
        os::Setenv(k, v);
    }
    ~setenvGuard() {
        if (had_) os::Setenv(k_, old_);
        else os::Unsetenv(k_);
    }

private:
    std::string k_;
    string old_;
    bool had_ = false;
};

void checkNotFound(const std::string& test) {
    auto [p, err] = exec::LookPath(test);
    EXPECT_TRUE(err) << test << ": error expected, got nil";
    EXPECT_EQ(p, "") << test << ": path returned should be \"\"";
}

}  // namespace

TEST(GoOsExec, LookPath) {
    TempDir tmp;
    std::string tmp_dir = join(tmp.path(), "testdir");
    ASSERT_FALSE(os::Mkdir(tmp_dir, 0777));
    const std::string executable = "execabs-test";
    ASSERT_FALSE(os::WriteFile(join(tmp_dir, executable), std::string_view("\1\2\3"), 0777));
    Chdir cd(tmp_dir);
    std::string orig_path = str(os::Getenv("PATH"));

    // Add "." to PATH so that LookPath looks in the working directory, and
    // try to trick it with "../testdir" too.
    for (std::string errdot : {"1", "0"}) {
        SCOPED_TRACE("GODEBUG=execerrdot=" + errdot);
        setenvGuard godebug("GODEBUG", "execerrdot=" + errdot + ",execwait=2");
        for (std::string dir : {".", "../testdir"}) {
            SCOPED_TRACE("PATH=" + dir);
            setenvGuard path("PATH", dir + ":" + orig_path);
            std::string good = dir + "/execabs-test";
            auto [found, err] = exec::LookPath(good);
            ASSERT_TRUE(!err && found.starts_with(good)) << "LookPath(" << good << ") = " << found << ", " << err;

            auto [_, err2] = exec::LookPath("execabs-test");
            if (errdot == "1") {
                ASSERT_TRUE(err2) << "LookPath didn't fail when finding a non-relative path";
                ASSERT_TRUE(errors::Is(err2, exec::ErrDot)) << "LookPath returned unexpected error: " << err2;
            } else {
                ASSERT_FALSE(err2) << "LookPath failed unexpectedly: " << err2;
            }

            auto cmd = exec::Command("execabs-test");
            if (errdot == "1") {
                ASSERT_TRUE(cmd->Err) << "Command didn't fail when finding a non-relative path";
                ASSERT_TRUE(errors::Is(cmd->Err, exec::ErrDot)) << "Command returned unexpected error: " << cmd->Err;
                cmd->Err = nullptr;
            } else {
                ASSERT_FALSE(cmd->Err) << "Command failed unexpectedly: " << cmd->Err;
            }

            // Without cmd.Err, the execution proceeds, and fails: it's not a
            // valid binary.
            error rerr = cmd->Run();
            ASSERT_TRUE(rerr) << "Run did not fail: expected exec error";
            ASSERT_FALSE(errors::Is(rerr, exec::ErrDot)) << "Run returned unexpected error ErrDot: " << rerr;
        }
    }

    {
        // The first entry of PATH is an absolute name of the working
        // directory: no ErrDot.
        SCOPED_TRACE("PATH=$PWD");
        setenvGuard path("PATH", tmp_dir + ":" + orig_path);
        std::string good = join(tmp_dir, "execabs-test");
        auto [found, err] = exec::LookPath(good);
        EXPECT_TRUE(!err && found.starts_with(good)) << "LookPath(" << good << ") = " << found << ", " << err;
        auto [found2, err2] = exec::LookPath("execabs-test");
        EXPECT_TRUE(!err2 && found2.starts_with(good)) << "LookPath(execabs-test) = " << found2 << ", " << err2;
        EXPECT_FALSE(exec::Command("execabs-test")->Err);
    }
    {
        // Control case: PATH doesn't include "." implicitly on Unix, so an
        // executable of the same name in a directory of PATH is found.
        SCOPED_TRACE("PATH=$OTHER");
        bool want_err_dot = false;
        {
            setenvGuard path("PATH", "");
            auto [found, err] = exec::LookPath("execabs-test");
            if (errors::Is(err, exec::ErrDot)) want_err_dot = true;
            else EXPECT_TRUE(err) << "with PATH='', LookPath(execabs-test) = " << found << "; want non-nil error";
        }
        TempDir other;
        ASSERT_FALSE(os::WriteFile(join(other.path(), executable), std::string_view("\1\2\3"), 0777));
        setenvGuard path("PATH", other.path() + ":" + orig_path);
        auto [found, err] = exec::LookPath("execabs-test");
        if (want_err_dot) {
            EXPECT_TRUE(found == executable && errors::Is(err, exec::ErrDot));
        } else {
            EXPECT_TRUE(found == join(other.path(), executable) && !err)
                << "LookPath(execabs-test) = " << found << ", " << err;
        }
    }
    auto checks = [] {
        checkNotFound("");
        checkNotFound(".");
        checkNotFound("abc/..");
        checkNotFound("..");
    };
    {
        SCOPED_TRACE("PATH=$OTHER2");
        checks();
    }
    {
        // PATH with an executable file that isn't a directory.
        SCOPED_TRACE("PATH=exe");
        setenvGuard path("PATH", str(go_helper::executable()));
        checks();
    }
    {
        SCOPED_TRACE("PATH=exe/xx");
        setenvGuard path("PATH", str(go_helper::executable()) + "/xx");
        checks();
    }
}

// env_test.go (an internal test of dedupEnvCase)

TEST(GoOsExec, DedupEnv) {
    struct test {
        bool no_case = false;
        bool nul_ok = false;
        std::vector<std::string> in, want;
        bool want_err = false;
    };
    using namespace std::string_literals;
    const std::vector<test> tests = {
        {.no_case = true, .in = {"k1=v1", "k2=v2", "K1=v3"}, .want = {"k2=v2", "K1=v3"}},
        {.no_case = false, .in = {"k1=v1", "K1=V2", "k1=v3"}, .want = {"K1=V2", "k1=v3"}},
        {.in = {"=a", "=b", "foo", "bar"}, .want = {"=b", "foo", "bar"}},
        // Go's #49886: preserve weird Windows keys with leading "=" signs.
        {.no_case = true, .in = {"=C:=C:\\golang", "=D:=D:\\tmp", "=D:=D:\\"}, .want = {"=C:=C:\\golang", "=D:=D:\\"}},
        // Go's #52436: preserve invalid key-value entries (for now).
        {.in = {"dodgy", "entries"}, .want = {"dodgy", "entries"}},
        // Filter out entries containing NULs.
        {.in = {"A=a\0b"s, "B=b", "C\0C=c"s}, .want = {"B=b"}, .want_err = true},
        // Plan 9 needs to preserve environment variables with NUL (#56544).
        {.nul_ok = true, .in = {"path=one\0two"s}, .want = {"path=one\0two"s}},
    };
    for (const test& tt : tests) {
        slice<string> in = slice<string>::make(0);
        for (auto& s : tt.in) in = append(in, string(s));
        auto [got, err] = exec::detail::dedup_env_case(tt.no_case, tt.nul_ok, in);
        std::vector<std::string> g;
        for (const string& s : got) g.emplace_back(s.view());
        EXPECT_TRUE(g == tt.want && static_cast<bool>(err) == tt.want_err)
            << "Dedup(" << tt.no_case << ", " << testing::PrintToString(tt.in) << ") = " << testing::PrintToString(g)
            << ", " << err;
    }
}

// go++'s own: filepath.Clean, which Cmd's PWD and LookPath use.
TEST(GoOsExec, CleanPath) {
    const std::vector<std::pair<std::string, std::string>> tests = {
        {"", "."},          {"abc", "abc"},       {"abc/def", "abc/def"}, {"a/b/c", "a/b/c"},
        {".", "."},         {"..", ".."},         {"../..", "../.."},     {"../../abc", "../../abc"},
        {"/abc", "/abc"},   {"/", "/"},           {"abc/", "abc"},        {"/abc/", "/abc"},
        {"abc//def", "abc/def"},                  {"./abc", "abc"},       {"abc/./def", "abc/def"},
        {"abc/def/..", "abc"}, {"abc/def/../..", "."}, {"/abc/def/../..", "/"}, {"abc/def/../../..", ".."},
        {"/abc/def/../../..", "/"}, {"abc/def/../../../ghi/jkl/../../../mno", "../../mno"},
        {"/../abc", "/abc"},
    };
    for (auto& [in, want] : tests) EXPECT_EQ(exec::detail::cleanPath(in), want) << in;
}
