// Ported from Go 1.27.3's os tests: the process tests of os_test.go,
// exec_unix_test.go, pidfd_linux_test.go and executable_test.go, with Go's
// test names. See go_os_test.cpp.
//
// Go's tests that run the test binary again select the child's part with
// -test.run and an environment variable; here the child is a helper
// command of go_helper.hpp. Tests that run the go tool run /bin/true or
// /bin/sleep instead.
#include "go_helper.hpp"
#include "go_os_test_util.hpp"

#include <gopp/fmt.hpp>
#include <gopp/os/exec.hpp>

#include <signal.h>
#include <sys/syscall.h>

#include <climits>

using namespace os_test;
using go_helper::helper_command;

namespace {

const bool helpers_registered = [] {
    // Go's TestMain with GO_OS_TEST_DRAIN_STDIN.
    go_helper::registerHelper("drainstdin", [](const std::vector<std::string>&) {
        os::Stdout.Close();
        io::Copy(io::Discard(), os::Stdin);
        os::Exit(0);
    });
    go_helper::registerHelper("getppid", [](const std::vector<std::string>&) { fmt::Print("{}", os::Getppid()); });
    go_helper::registerHelper("execpath", [](const std::vector<std::string>&) {
        os::Chdir("/");
        auto [ep, err] = os::Executable();
        if (err) fmt::Print(os::Stderr, "ERROR: {}", err);
        else fmt::Print(os::Stderr, "{}", ep);
        os::Exit(0);
    });
    go_helper::registerHelper("sleepminute", [](const std::vector<std::string>&) { time::Sleep(time::Minute); });
    return true;
}();

std::string baseName(const std::string& p) { return p.substr(p.rfind('/') + 1); }
std::string dirName(const std::string& p) {
    std::size_t i = p.rfind('/');
    return i == 0 ? "/" : p.substr(0, i);
}

// Go's testStartProcess.
void test_start_process(const std::string& dir, const std::string& cmd, std::vector<string> args,
                        const std::string& expect) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << "Pipe: " << err;
    os::ProcAttr attr{.Dir = string(dir), .Files = {nullptr, w, ptr<File>(&os::Stderr)}};
    auto [p, serr] = os::StartProcess(cmd, args, attr);
    ASSERT_FALSE(serr) << "StartProcess: " << serr;
    w->Close();

    strings::Builder b;
    io::Copy(b, *r);
    r->Close();
    std::string output(b.view());

    auto [fi1, e1] = os::Stat(str(strings::TrimSpace(string(output))));
    auto [fi2, e2] = os::Stat(expect);
    EXPECT_TRUE(fi1 && fi2 && os::SameFile(fi1, fi2))
        << "exec " << cmd << " returned " << output << " wanted " << expect;
    p->Wait();
}

// Go's testKillProcess: a child that hangs until stdin is closed.
void test_kill_process(const std::function<void(const ptr<os::Process>&)>& process_killer) {
    auto cmd = helper_command("drainstdin");
    auto [stdout, err] = cmd->StdoutPipe();
    ASSERT_FALSE(err) << err;
    auto [stdin, ierr] = cmd->StdinPipe();
    ASSERT_FALSE(ierr) << ierr;
    ASSERT_FALSE(cmd->Start()) << "Failed to start test process";

    // Wait for the process to be started: it closes its stdout first.
    io::Copy(io::Discard(), *stdout);
    process_killer(cmd->Process);

    EXPECT_TRUE(cmd->Wait()) << "Test process succeeded, but expected to fail";
    stdin->Close();  // keep stdin alive until the process has finished dying
}

// Go's runBinHostname.
std::string run_bin_hostname() {
    auto [r, w, err] = os::Pipe();
    EXPECT_FALSE(err) << err;
    auto [path, lerr] = os::exec::LookPath("hostname");
    if (lerr) return {};
    std::vector<string> argv{"hostname"};
    auto [p, serr] = os::StartProcess(path, argv, os::ProcAttr{.Files = {nullptr, w, ptr<File>(&os::Stderr)}});
    EXPECT_FALSE(serr) << serr;
    w->Close();
    std::string host = read_all(*r);
    r->Close();
    if (p) {
        EXPECT_FALSE(p->Wait().second);
    }
    if (!host.empty() && host.back() == '\n') host.pop_back();
    EXPECT_NE(host, "") << "/bin/hostname produced no output";
    return host;
}

// pidfd_send_signal(2), for the pidfd tests.
error pidfd_send_signal(uintptr pidfd, syscall::Signal sig) {
    if (::syscall(SYS_pidfd_send_signal, static_cast<int>(pidfd), sig.value, nullptr, 0) < 0) {
        return syscall::errnoError(errno);
    }
    return nullptr;
}

}  // namespace

TEST(GoOs, StartProcess) {
    auto [cmd, err] = os::exec::LookPath("pwd");
    ASSERT_FALSE(err) << "Can't find pwd: " << err;
    std::string c = str(cmd);
    std::string cmddir = dirName(c), cmdbase = baseName(c);
    {
        SCOPED_TRACE("absolute");
        test_start_process("/", c, {string(cmdbase)}, "/");
    }
    {
        SCOPED_TRACE("relative");
        test_start_process(cmddir, cmdbase, {string(cmdbase)}, cmddir);
    }
}

TEST(GoOs, Hostname) {
    auto [hostname, err] = os::Hostname();
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(hostname.empty()) << "Hostname returned empty string and no error";
    ASSERT_EQ(hostname.find('\0'), string::npos) << "unexpected zero byte in hostname: " << hostname;

    if (auto [_, lerr] = os::exec::LookPath("hostname"); lerr) GTEST_SKIP() << "test requires hostname but it does not exist";
    // Hostname may return a fully qualified domain name, and /bin/hostname
    // only its first component.
    std::string want = run_bin_hostname();
    if (hostname != want) {
        auto [host, _, ok] = strings::Cut(hostname, ".");
        EXPECT_TRUE(ok && host == want) << "Hostname() = " << hostname << ", want " << want;
    }
}

TEST(GoOs, NilProcessStateString) {
    // A nil ptr<ProcessState> has no methods to call in C++.
    GTEST_SKIP() << "C++ can't call String through a nil ProcessState";
}

TEST(GoOs, KillStartProcess) {
    test_kill_process([](const ptr<os::Process>& p) { ASSERT_FALSE(p->Kill()) << "Failed to kill test process"; });
}

TEST(GoOs, Getppid) {
    auto cmd = helper_command("getppid");
    // Verify that Getppid() from the forked process reports our process id.
    auto [output, err] = cmd->CombinedOutput();
    ASSERT_FALSE(err) << "Failed to spawn child process: " << err << " " << asStringView(output);
    EXPECT_EQ(asStringView(output), std::to_string(os::Getpid()));
}

TEST(GoOs, KillFindProcess) {
    test_kill_process([](const ptr<os::Process>& p) {
        auto [p2, err] = os::FindProcess(p->Pid);
        ASSERT_FALSE(err) << "Failed to find test process: " << err;
        ASSERT_FALSE(p2->Kill()) << "Failed to kill test process";
    });
}

TEST(GoOs, UserCacheDir) {
    auto [dir, err] = os::UserCacheDir();
    if (err) GTEST_SKIP() << err;
    ASSERT_FALSE(dir.empty()) << "UserCacheDir returned \"\"; want non-empty path or error";
    auto [fi, serr] = os::Stat(dir);
    if (serr) {
        if (os::IsNotExist(serr)) return;
        FAIL() << serr;
    }
    EXPECT_TRUE(fi->IsDir()) << "dir " << dir << " is not directory";
}

namespace {

// Go's t.Setenv: sets an environment variable until the end of the test.
class Setenv {
public:
    Setenv(const std::string& key, const std::string& value) : key_(key) {
        std::tie(old_, had_) = os::LookupEnv(key);
        EXPECT_FALSE(os::Setenv(key, value));
    }
    ~Setenv() {
        if (had_) os::Setenv(key_, old_);
        else os::Unsetenv(key_);
    }

private:
    std::string key_;
    string old_;
    bool had_ = false;
};

}  // namespace

TEST(GoOs, UserCacheDirXDGConfigDirEnvVar) {
    auto [wd, err] = os::Getwd();
    ASSERT_FALSE(err) << err;
    {
        Setenv e("XDG_CACHE_HOME", str(wd));
        auto [dir, derr] = os::UserCacheDir();
        ASSERT_FALSE(derr) << derr;
        EXPECT_EQ(dir, wd) << "UserCacheDir returned " << dir << "; want the value of $XDG_CACHE_HOME";
    }
    Setenv e("XDG_CACHE_HOME", "some-dir");
    EXPECT_TRUE(os::UserCacheDir().second) << "UserCacheDir succeeded though $XDG_CACHE_HOME contains a relative path";
}

TEST(GoOs, UserConfigDir) {
    auto [dir, err] = os::UserConfigDir();
    if (err) GTEST_SKIP() << err;
    ASSERT_FALSE(dir.empty()) << "UserConfigDir returned \"\"; want non-empty path or error";
    auto [fi, serr] = os::Stat(dir);
    if (serr) {
        if (os::IsNotExist(serr)) return;
        FAIL() << serr;
    }
    EXPECT_TRUE(fi->IsDir()) << "dir " << dir << " is not directory";
}

TEST(GoOs, UserConfigDirXDGConfigDirEnvVar) {
    auto [wd, err] = os::Getwd();
    ASSERT_FALSE(err) << err;
    {
        Setenv e("XDG_CONFIG_HOME", str(wd));
        auto [dir, derr] = os::UserConfigDir();
        ASSERT_FALSE(derr) << derr;
        EXPECT_EQ(dir, wd) << "UserConfigDir returned " << dir << "; want the value of $XDG_CONFIG_HOME";
    }
    Setenv e("XDG_CONFIG_HOME", "some-dir");
    EXPECT_TRUE(os::UserConfigDir().second) << "UserConfigDir succeeded though $XDG_CONFIG_HOME contains a relative path";
}

TEST(GoOs, UserHomeDir) {
    auto [dir, err] = os::UserHomeDir();
    ASSERT_FALSE(dir.empty() && !err) << "UserHomeDir returned an empty string but no error";
    if (err) GTEST_SKIP() << err;
    auto [fi, serr] = os::Stat(dir);
    if (serr) {
        if (os::IsNotExist(serr)) return;
        FAIL() << serr;
    }
    EXPECT_TRUE(fi->IsDir()) << "dir " << dir << " is not directory";
}

// exec_unix_test.go

TEST(GoOs, ErrProcessDone) {
    auto [p, err] = os::StartProcess("/bin/true", std::vector<string>{"true"}, os::ProcAttr{});
    ASSERT_FALSE(err) << "starting test process: " << err;
    p->Wait();
    EXPECT_EQ(p->Signal(os::Kill), os::ErrProcessDone);
}

TEST(GoOs, ProcessAlreadyDone) {
    // Lookup of a process that does not exist at time of lookup.
    // Theoretically MaxInt32 is a valid PID, but the chance of it actually
    // being used is extremely unlikely.
    auto [p, err] = os::FindProcess(INT_MAX);
    ASSERT_FALSE(err) << "FindProcess(math.MaxInt32) got err " << err << ", want nil";
    auto [ps, werr] = p->Wait();
    EXPECT_TRUE(errors::Is(werr, syscall::ECHILD_)) << "Wait() got err " << werr << ", want ECHILD";
    EXPECT_FALSE(p->Release());
}

TEST(GoOs, UNIXProcessAlive) {
    auto [p, err] = os::StartProcess("/bin/sleep", std::vector<string>{"sleep", "1"}, os::ProcAttr{});
    if (err) GTEST_SKIP() << "starting test process: " << err;
    auto [proc, ferr] = os::FindProcess(p->Pid);
    EXPECT_FALSE(ferr) << "OS reported error for running process: " << ferr;
    EXPECT_FALSE(proc->Signal(syscall::Signal(0))) << "OS reported error for running process";
    p->Kill();
    p->Wait();
}

TEST(GoOs, ProcessBadPID) {
    auto [p, err] = os::FindProcess(-1);
    ASSERT_FALSE(err) << "unexpected FindProcess error: " << err;
    EXPECT_TRUE(p->Signal(syscall::Signal(0))) << "p.Signal succeeded unexpectedly";
}

// pidfd_linux_test.go

TEST(GoOs, FindProcessViaPidfd) {
    if (!os::detail::pidfd_works()) GTEST_SKIP() << "pidfd not available";
    auto [p, err] = os::StartProcess("/bin/true", std::vector<string>{"true"}, os::ProcAttr{});
    ASSERT_FALSE(err) << "starting test process: " << err;
    p->Wait();

    // Use the pid of a process that doesn't exist.
    auto [proc, ferr] = os::FindProcess(p->Pid);
    // FindProcess never returns errors on Unix, nor a nil Process.
    ASSERT_FALSE(ferr) << "FindProcess: got error " << ferr << ", want <nil>";
    ASSERT_TRUE(proc) << "FindProcess: got nil, want non-nil";
    // Go checks proc.Status() == StatusDone, an internal method; the
    // public methods below tell the same.

    // All of Process's methods work as expected with a "done" Process.
    EXPECT_EQ(proc->Kill(), os::ErrProcessDone);
    EXPECT_EQ(proc->Signal(os::Kill), os::ErrProcessDone);
    auto [_, werr] = proc->Wait();
    EXPECT_TRUE(errors::Is(werr, syscall::ECHILD_)) << "Wait: got " << werr;
    // Release never returns errors on Unix.
    EXPECT_FALSE(proc->Release());
}

TEST(GoOs, StartProcessWithPidfd) {
    if (!os::detail::pidfd_works()) GTEST_SKIP() << "pidfd not available";
    int pidfd = -1;
    syscall::SysProcAttr sys;
    sys.PidFD = &pidfd;
    auto [p, err] = os::StartProcess("/bin/true", std::vector<string>{"true"}, os::ProcAttr{.Sys = sys});
    ASSERT_FALSE(err) << "starting test process: " << err;
    auto [_, werr] = p->Wait();
    EXPECT_FALSE(werr) << "Wait: got " << werr << ", want <nil>";
    // The pidfd is still valid.
    error serr = pidfd_send_signal(static_cast<uintptr>(pidfd), syscall::Signal(0));
    EXPECT_TRUE(errors::Is(serr, syscall::ESRCH_)) << "SendSignal: got " << serr << ", want ESRCH";
    ::close(pidfd);
}

TEST(GoOs, PidfdLeak) {
    std::string exe = str(go_helper::executable());
    // Find the next 10 descriptors. More than one are needed in practice:
    // the pidfd winds up not being the next descriptor.
    constexpr int count = 10;
    int want[count], got[count];
    for (int& d : want) {
        d = ::open(exe.c_str(), O_RDONLY | O_CLOEXEC);
        ASSERT_GE(d, 0);
    }
    for (int d : want) ::close(d);

    // Start a process 10 times. It must be an absolute path, or no process
    // would start at all.
    for (int i = 0; i < 10; i++) os::exec::Command("/noSuchExecutable")->Run();

    for (int& d : got) {
        d = ::open(exe.c_str(), O_RDONLY | O_CLOEXEC);
        ASSERT_GE(d, 0);
    }
    for (int d : got) ::close(d);
    // Allow some slack for the runtime's descriptors and the like.
    EXPECT_LE(got[count - 1], want[count - 1] + 5) << "got descriptor " << got[count - 1] << ", want "
                                                   << want[count - 1];
}

TEST(GoOs, ProcessWithHandleLinux) {
    bool have_pidfd = os::detail::pidfd_works();
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto cmd = go_helper::helperCommandContext(ctx, "sleepminute");
    ASSERT_FALSE(cmd->Start());

    const syscall::Signal sig = syscall::SIGINT_;
    bool called = false;
    error err = cmd->Process->WithHandle([&](uintptr pidfd) {
        called = true;
        // The pidfd is valid: terminate the child with it.
        EXPECT_FALSE(pidfd_send_signal(pidfd, sig)) << "PidFDSendSignal: got error, want nil";
    });
    // WithHandle fails without pidfds, and succeeds with them.
    if (have_pidfd) {
        EXPECT_FALSE(err) << "WithHandle: got error " << err << ", want nil";
    } else {
        EXPECT_TRUE(err) << "WithHandle: got nil, want error";
    }
    EXPECT_EQ(have_pidfd, called);
    if (called) {
        // Wait for the child, to check that it worked.
        EXPECT_TRUE(cmd->Wait()) << "Wait: want error, got nil";
        syscall::WaitStatus st = cmd->ProcessState->Sys();
        EXPECT_TRUE(st.Signaled()) << "ProcessState: want Signaled";
        EXPECT_EQ(st.Signal(), sig) << "ProcessState.Signal: want " << sig << ", got " << st.Signal();
        // WithHandle now returns ErrProcessDone.
        called = false;
        err = cmd->Process->WithHandle([&](uintptr) { called = true; });
        EXPECT_EQ(err, os::ErrProcessDone);
        EXPECT_FALSE(called);
    } else {
        cmd->Process->Kill();
        cmd->Wait();
    }
    cancel();
}

// executable_test.go

TEST(GoOs, Executable) {
    std::string ep = str(go_helper::executable());
    // Run the child as "dir/prog", from dir.
    std::string dir = dirName(dirName(ep));
    std::string fn = ep.substr(dir.size() + (dir == "/" ? 0 : 1));

    auto cmd = helper_command("execpath");
    cmd->Dir = string(dir);
    cmd->Path = string(fn);
    // Forge argv[0] for the child, to check that the real path of the
    // executable is found without it.
    cmd->Args[0] = "-";
    auto [out, err] = cmd->CombinedOutput();
    ASSERT_FALSE(err) << "exec(self) failed: " << err;
    std::string outs(asStringView(out));
    ASSERT_TRUE(!outs.empty() && outs[0] == '/') << "Child returned " << outs << ", want an absolute path";
    auto [fi1, e1] = os::Stat(outs);
    auto [fi2, e2] = os::Stat(ep);
    EXPECT_TRUE(!e1 && !e2 && os::SameFile(fi1, fi2)) << "Child returned " << outs << ", not the same file as " << ep;
}

TEST(GoOs, ExecutableDeleted) {
    // Go builds a helper program, runs it, and removes its file while it
    // runs; that needs the go tool.
    GTEST_SKIP() << "needs to build a Go program";
}

// go++'s own: the paths taken without pidfds, as on kernels before 5.4.

namespace {

struct withoutPidfd {
    withoutPidfd() { os::detail::pidfd_disabled.store(true); }
    ~withoutPidfd() { os::detail::pidfd_disabled.store(false); }
};

}  // namespace

TEST(GoOs, ProcessWithoutPidfd) {
    withoutPidfd guard;
    // Exit status.
    auto [p, err] = os::StartProcess("/bin/sh", std::vector<string>{"sh", "-c", "exit 3"}, os::ProcAttr{});
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(p->WithHandle([](uintptr) {}), os::ErrNoHandle);
    auto [ps, werr] = p->Wait();
    ASSERT_FALSE(werr) << werr;
    EXPECT_EQ(ps->ExitCode(), 3);
    EXPECT_EQ(ps->Pid(), p->Pid);
    EXPECT_EQ(ps->String(), "exit status 3");
    EXPECT_EQ(p->Signal(os::Kill), os::ErrProcessDone);
    auto [_, werr2] = p->Wait();
    EXPECT_TRUE(werr2) << "second Wait succeeded";

    // Killed, from fibers that wait meanwhile.
    auto [s, serr] = os::StartProcess("/bin/sleep", std::vector<string>{"sleep", "10"}, os::ProcAttr{});
    ASSERT_FALSE(serr) << serr;
    auto done = makeChan<ptr<os::ProcessState>>(1);
    go([s, done] { done.send(s->Wait().first); });
    time::Sleep(10 * time::Millisecond);
    EXPECT_FALSE(s->Signal(syscall::SIGTERM_));
    auto st = done.recv();
    ASSERT_TRUE(st);
    EXPECT_FALSE(st->Exited());
    EXPECT_EQ(st->Sys().Signal(), syscall::SIGTERM_);
    EXPECT_EQ(st->String(), "signal: terminated");
    EXPECT_EQ(st->ExitCode(), -1);

    // FindProcess without a pidfd uses the pid.
    auto [f, ferr] = os::FindProcess(os::Getpid());
    ASSERT_FALSE(ferr);
    EXPECT_FALSE(f->Signal(syscall::Signal(0)));
    EXPECT_EQ(f->WithHandle([](uintptr) {}), os::ErrNoHandle);
    EXPECT_FALSE(f->Release());
    EXPECT_EQ(f->Pid, -1);
}

TEST(GoOs, ProcessStateUsage) {
    // A process that uses some CPU: its times are reported.
    auto [p, err] = os::StartProcess(
        "/bin/sh", std::vector<string>{"sh", "-c", "i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done"}, os::ProcAttr{});
    ASSERT_FALSE(err) << err;
    auto [ps, werr] = p->Wait();
    ASSERT_FALSE(werr) << werr;
    EXPECT_TRUE(ps->Success());
    EXPECT_TRUE(ps->Exited());
    EXPECT_GT((ps->UserTime() + ps->SystemTime()).ns, 0);
    EXPECT_GT(ps->SysUsage()->Maxrss, 0);
}

TEST(GoOs, WaitManyFromFibers) {
    // Many children waited for at once, each by its own fiber: with pidfds
    // they wait in the poller, not on threads.
    constexpr int n = 50;
    sync::WaitGroup wg;
    std::atomic<int> ok{0};
    for (int i = 0; i < n; i++) {
        wg.Add(1);
        go([&] {
            auto [p, err] = os::StartProcess("/bin/sleep", std::vector<string>{"sleep", "0.2"}, os::ProcAttr{});
            if (!err) {
                auto [ps, werr] = p->Wait();
                if (!werr && ps->Success()) ok++;
            }
            wg.Done();
        });
    }
    wg.Wait();
    EXPECT_EQ(ok.load(), n);
}
