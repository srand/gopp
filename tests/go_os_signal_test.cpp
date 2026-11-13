// Ported from Go 1.27.3's os/signal/signal_test.go, with Go's test names.
//
// The tests that run the test binary again with flags (-check_sighup_ignored,
// -send_uncaught_sighup, -check_notify_ctx, GO_TEST_ATOMIC_STOP) run helper
// commands of go_helper.hpp instead. TestSignalTrace (runtime/trace) and
// TestAllThreadsSyscallSignals (syscall.AllThreadsSyscall) aren't ported:
// go++ has neither. The NotifyContext tests don't check the context's
// String: go++'s contexts have none.
//
// One difference with Go: a signal nobody is notified of keeps its default
// action. Go's runtime ignores SIGUSR1 and SIGWINCH-like signals that
// aren't notified; in go++, SIGUSR1 then ends the program, as in C. So
// TestStop doesn't send SIGUSR1 while nobody is notified of it.
#include "go_helper.hpp"
#include "go_os_test_util.hpp"

#include <gopp/context.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/os/signal.hpp>

#include <cstdlib>
#include <sstream>

using namespace os_test;
using go_helper::helper_command;

namespace {

// An upper bound on how long signals take to be delivered.
const time::Duration settle_time = 100 * time::Millisecond;
// An absurdly long time to wait for signals, to avoid flakes.
const time::Duration fatal_waiting_time = 30 * time::Second;

// Go's waitSig1, returning an error message instead of failing.
std::string wait_sig1(const chan<os::Signal>& c, os::Signal sig, bool all) {
    auto start = time::Now();
    while (time::Since(start) < fatal_waiting_time) {
        std::string err;
        bool done = false;
        select(recv(c,
                    [&](os::Signal s) {
                        if (s == sig) {
                            done = true;
                            return;
                        }
                        // With all signals notified, SIGURG may come at any
                        // time (Go's runtime uses it for preemption).
                        if (!all || s != syscall::SIGURG_) {
                            err = "signal was " + str(s.String()) + ", want " + str(sig.String());
                            done = true;
                        }
                    }),
               recv(time::After(settle_time / 10), [] {}));
        if (done) return err;
    }
    return "timeout waiting for " + str(sig.String());
}

void wait_sig(const chan<os::Signal>& c, os::Signal sig) {
    std::string err = wait_sig1(c, sig, false);
    ASSERT_EQ(err, "");
}

void wait_sig_all(const chan<os::Signal>& c, os::Signal sig) {
    std::string err = wait_sig1(c, sig, true);
    ASSERT_EQ(err, "");
}

// Waits until all pending signals have reasonably been delivered.
void quiesce() {
    auto start = time::Now();
    while (time::Since(start) < settle_time) time::Sleep(settle_time / 10);
}

void kill_self(syscall::Signal sig) { syscall::Kill(syscall::Getpid(), sig); }

// Go's TestStop for one signal, as a function the helper "stop" runs too;
// returns an error message. send_uncaught_sighup: 1 sends SIGHUP before
// Notify, 2 after Stop.
std::string test_stop(syscall::Signal sig, int send_uncaught_sighup) {
    // If the signal isn't ignored, send it before Notify, to check the
    // default handling: SIGWINCH is ignored, SIGHUP may end the program,
    // as the argument says. (Not SIGUSR1, which ends a go++ program.)
    bool may_have_blocked_signal = false;
    if (!os::signal::Ignored(sig) && sig != syscall::SIGUSR1_ && (sig != syscall::SIGHUP_ || send_uncaught_sighup == 1)) {
        kill_self(sig);
        quiesce();
        may_have_blocked_signal = true;
    }

    // Ask for the signal, and send it again.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, sig);
    kill_self(sig);
    if (std::string err = wait_sig1(c, sig, false); !err.empty()) return err;

    if (may_have_blocked_signal) {
        // A queued initial signal may have come too: let it clear.
        quiesce();
        os::Signal s;
        c.try_recv(s);
    }

    // Stop watching for the signal and send it again.
    os::signal::Stop(c);
    if (sig != syscall::SIGUSR1_ && (sig != syscall::SIGHUP_ || send_uncaught_sighup == 2)) {
        kill_self(sig);
        quiesce();
        os::Signal s;
        if (c.try_recv(s)) return "unexpected signal " + str(s.String());
        // A signal that was blocked comes when it's unblocked.
        os::signal::Notify(c, sig);
        quiesce();
        os::signal::Stop(c);
    }
    return "";
}

std::string run_test_stop(int send_uncaught_sighup) {
    for (syscall::Signal sig : {syscall::SIGWINCH_, syscall::SIGHUP_, syscall::SIGUSR1_}) {
        if (std::string err = test_stop(sig, send_uncaught_sighup); !err.empty()) return sig.String().str() + ": " + err;
    }
    return "";
}

// Go's atomicStopTestProgram: either catches SIGINT or dies of it.
void atomic_stop_test_program() {
    if (os::signal::Ignored(syscall::SIGINT_)) {
        fmt::Println("SIGINT is ignored");
        os::Exit(1);
    }
    constexpr int tries = 10;
    const time::Duration timeout = 2 * time::Second;
    int pid = syscall::Getpid();
    bool printed = false;
    for (int i = 0; i < tries; i++) {
        auto cs = make_chan<os::Signal>(1);
        os::signal::Notify(cs, syscall::SIGINT_);

        sync::WaitGroup wg;
        wg.Add(1);
        go([&] {
            os::signal::Stop(cs);
            wg.Done();
        });

        syscall::Kill(pid, syscall::SIGINT_);

        // We should either die from SIGINT or get a notification on cs.
        // If neither happens, we dropped the signal.
        select(recv(cs, [] {}), recv(time::After(timeout), [&] {
                   if (!printed) {
                       fmt::Print("lost signal on tries:");
                       printed = true;
                   }
                   fmt::Print(" {}", i);
               }));
        wg.Wait();
    }
    if (printed) fmt::Print("\n");
    os::Exit(0);
}

const bool helpers_registered = [] {
    go_helper::register_helper("check_sighup_ignored", [](const std::vector<std::string>&) {
        if (!os::signal::Ignored(syscall::SIGHUP_)) {
            fmt::Println("SIGHUP is not ignored.");
            os::Exit(1);
        }
        fmt::Println("SIGHUP is ignored.");
    });
    // TestStop with -send_uncaught_sighup=args[0].
    go_helper::register_helper("stop", [](const std::vector<std::string>& args) {
        std::string err = run_test_stop(std::atoi(args.at(0).c_str()));
        if (!err.empty()) {
            fmt::Println("{}", err);
            os::Exit(1);
        }
        fmt::Println("PASS");
    });
    go_helper::register_helper("atomic_stop", [](const std::vector<std::string>&) { atomic_stop_test_program(); });
    // TestNotifyContextNotifications with -ctx_notify_times=args[0].
    go_helper::register_helper("notify_ctx", [](const std::vector<std::string>& args) {
        auto [ctx, stop] = os::signal::NotifyContext(context::Background(), syscall::SIGINT_);
        // Stop mustn't be called when a signal is received: wait for a
        // number of signals to check that.
        sync::WaitGroup wg;
        int n = std::atoi(args.at(0).c_str());
        wg.Add(n);
        for (int i = 0; i < n; i++) {
            go([&wg] {
                kill_self(syscall::SIGINT_);
                wg.Done();
            });
        }
        wg.Wait();
        ctx->Done().recv();
        if (string got = context::Cause(ctx).Error(); got != "interrupt signal received") {
            fmt::Println("context.Cause(ctx) = {}, want interrupt signal received", got);
            os::Exit(1);
        }
        fmt::Println("received SIGINT");
        // Give the other signals time to come in: they must be ignored, as
        // stop isn't called.
        time::Sleep(settle_time);
    });
    return true;
}();

}  // namespace

// Test that basic signal handling works.
TEST(GoOsSignal, Signal) {
    // Ask for SIGHUP.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGHUP_);

    // Send this process a SIGHUP.
    kill_self(syscall::SIGHUP_);
    wait_sig(c, syscall::SIGHUP_);

    // Ask for everything we can get. The buffer has more than 1 in case the
    // runtime sends SIGURG (Go's does).
    auto c1 = make_chan<os::Signal>(10);
    os::signal::Notify(c1);
    // Stop relaying SIGURG signals. See Go's #49724.
    os::signal::Reset(syscall::SIGURG_);

    // Send this process a SIGWINCH.
    kill_self(syscall::SIGWINCH_);
    wait_sig_all(c1, syscall::SIGWINCH_);

    // Send two more SIGHUPs, to make sure that they get delivered on c1
    // and that not reading from c does not block everything.
    kill_self(syscall::SIGHUP_);
    wait_sig_all(c1, syscall::SIGHUP_);
    kill_self(syscall::SIGHUP_);
    wait_sig_all(c1, syscall::SIGHUP_);

    // The first SIGHUP should be waiting for us on c.
    wait_sig(c, syscall::SIGHUP_);

    os::signal::Stop(c1);
    os::signal::Stop(c);
}

TEST(GoOsSignal, Stress) {
    // Go runs 3s, or 100ms in short mode.
    const time::Duration dur = 300 * time::Millisecond;
    auto sig = make_chan<os::Signal>(1);
    os::signal::Notify(sig, syscall::SIGUSR1_);

    go([sig, dur] {
        auto stop = time::After(dur);
        for (;;) {
            bool stopped = false;
            select(recv(stop, [&] { stopped = true; }), default_case([] {
                       kill_self(syscall::SIGUSR1_);
                       runtime::Gosched();
                   }));
            if (stopped) {
                // Allow enough time for all signals to be delivered before
                // we stop listening for them.
                quiesce();
                os::signal::Stop(sig);
                // When Stop returns, c receives no more signals: closing it
                // now would show a send after close.
                sig.close();
                return;
            }
        }
    });

    for (auto s : sig) {
        // Receive signals until the sender closes sig.
        (void)s;
    }
}

namespace {

void test_cancel(bool ignore) {
    // Ask to be notified on c1 when a SIGWINCH is received.
    auto c1 = make_chan<os::Signal>(1);
    os::signal::Notify(c1, syscall::SIGWINCH_);
    // Ask to be notified on c2 when a SIGHUP is received.
    auto c2 = make_chan<os::Signal>(1);
    os::signal::Notify(c2, syscall::SIGHUP_);

    // Send this process a SIGWINCH and wait for notification on c1.
    kill_self(syscall::SIGWINCH_);
    wait_sig(c1, syscall::SIGWINCH_);

    // Send this process a SIGHUP and wait for notification on c2.
    kill_self(syscall::SIGHUP_);
    wait_sig(c2, syscall::SIGHUP_);

    // Ignore, or reset the signal handlers for, SIGWINCH and SIGHUP.
    // Either way, this should undo both calls to Notify above.
    if (ignore) {
        os::signal::Ignore(syscall::SIGWINCH_, syscall::SIGHUP_);
        // Reset is documented to undo Notify, not Ignore.
    } else {
        os::signal::Reset(syscall::SIGWINCH_, syscall::SIGHUP_);
    }

    // Send this process a SIGWINCH. It should be ignored.
    kill_self(syscall::SIGWINCH_);
    // If ignoring, send this process a SIGHUP. It should be ignored.
    if (ignore) kill_self(syscall::SIGHUP_);

    quiesce();

    os::Signal s;
    EXPECT_FALSE(c1.try_recv(s)) << "unexpected signal " << s;
    EXPECT_FALSE(c2.try_recv(s)) << "unexpected signal " << s;

    // One or both of the signals may have been blocked for this process
    // by the calling process. Discard any queued signals now to avoid
    // interfering with other tests.
    os::signal::Notify(c1, syscall::SIGWINCH_);
    os::signal::Notify(c2, syscall::SIGHUP_);
    quiesce();
    os::signal::Stop(c1);
    os::signal::Stop(c2);
}

}  // namespace

// Test that Reset cancels registration for listed signals on all channels.
TEST(GoOsSignal, Reset) { test_cancel(false); }

// Test that Ignore cancels registration for listed signals on all channels.
TEST(GoOsSignal, Ignore) {
    test_cancel(true);
    // Go leaves SIGHUP ignored here; give it back its default, so that
    // the next tests see the program as it started.
    os::signal::Reset(syscall::SIGWINCH_);
}

// Test that Ignored correctly detects changes to the ignored status of a
// signal.
TEST(GoOsSignal, Ignored) {
    // Ask to be notified on SIGWINCH.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGWINCH_);

    // If we're being notified, then the signal should not be ignored.
    EXPECT_FALSE(os::signal::Ignored(syscall::SIGWINCH_)) << "expected SIGWINCH to not be ignored.";
    os::signal::Stop(c);
    os::signal::Ignore(syscall::SIGWINCH_);

    // We're no longer paying attention to this signal.
    EXPECT_TRUE(os::signal::Ignored(syscall::SIGWINCH_)) << "expected SIGWINCH to be ignored when explicitly ignoring it.";

    os::signal::Reset();
}

// Test that Ignored(SIGHUP) correctly detects whether it is being run
// under nohup.
TEST(GoOsSignal, DetectNohup) {
    // Ask for SIGHUP, so that the child doesn't have it ignored even if
    // this test runs under nohup. We have no intention of reading from c.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGHUP_);
    {
        auto [out, err] = helper_command("check_sighup_ignored")->CombinedOutput();
        EXPECT_TRUE(err) << "ran test with -check_sighup_ignored and it succeeded: expected failure.\nOutput:\n"
                         << as_string_view(out);
    }
    os::signal::Stop(c);

    // Again, this time with nohup, assuming we can find it.
    if (os::Stat("/usr/bin/nohup").second) {
        os::signal::Reset();
        GTEST_SKIP() << "cannot find nohup; skipping second half of test";
    }
    os::signal::Ignore(syscall::SIGHUP_);
    os::Remove("nohup.out");
    auto cmd = helper_command("check_sighup_ignored");
    cmd->Args = slice<string>{"/usr/bin/nohup", cmd->Path, "check_sighup_ignored"};
    cmd->Path = "/usr/bin/nohup";
    auto [out, err] = cmd->CombinedOutput();
    auto [data, _] = os::ReadFile("nohup.out");
    os::Remove("nohup.out");
    EXPECT_FALSE(err) << "ran test with -check_sighup_ignored under nohup and it failed: expected success.\nError: "
                      << err << "\nOutput:\n"
                      << as_string_view(out) << as_string_view(data);
    os::signal::Reset();
}

// Test that Stop cancels the channel's registrations.
TEST(GoOsSignal, Stop) {
    for (syscall::Signal sig : {syscall::SIGWINCH_, syscall::SIGHUP_, syscall::SIGUSR1_}) {
        SCOPED_TRACE(sig.String().str());
        EXPECT_EQ(test_stop(sig, 0), "");
    }
}

// Test that when run under nohup, an uncaught SIGHUP does not kill the
// program.
TEST(GoOsSignal, Nohup) {
    // Ask for SIGHUP, so that the child doesn't have it ignored even if
    // this test runs under nohup. We have no intention of reading from c.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGHUP_);

    // Without nohup, the uncaught SIGHUP ends the child, before (1) or
    // after (2) it's notified.
    for (int i = 1; i <= 2; i++) {
        auto [out, err] = helper_command("stop", {std::to_string(i)})->CombinedOutput();
        EXPECT_TRUE(err) << "ran test with -send_uncaught_sighup=" << i
                         << " and it succeeded: expected failure.\nOutput:\n"
                         << as_string_view(out);
    }
    os::signal::Stop(c);

    // Under nohup, the uncaught SIGHUPs are ignored.
    auto [nohup, lerr] = os::exec::LookPath("nohup");
    if (lerr) GTEST_SKIP() << "cannot find nohup; skipping second half of test";
    for (int i = 1; i <= 2; i++) {
        auto cmd = helper_command("stop", {std::to_string(i)});
        cmd->Args = slice<string>{"nohup", cmd->Path, "stop", string(std::to_string(i))};
        cmd->Path = nohup;
        auto [out, err] = cmd->CombinedOutput();
        EXPECT_FALSE(err) << "ran test with -send_uncaught_sighup=" << i
                          << " under nohup and it failed: expected success.\nError: " << err << "\nOutput:\n"
                          << as_string_view(out);
    }
}

// Test that SIGCONT works (Go's issue 8953).
// Go's TestSIGCONT (SIGCONT is a C macro).
TEST(GoOsSignal, SIGCONT_) {
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGCONT_);
    kill_self(syscall::SIGCONT_);
    wait_sig(c, syscall::SIGCONT_);
    os::signal::Stop(c);
}

// Test race between stopping and receiving a signal (Go's issue 14571).
TEST(GoOsSignal, AtomicStop) {
    // Notify for SIGINT before starting the child, so that SIGINT isn't
    // ignored for the child.
    auto cs = make_chan<os::Signal>(1);
    os::signal::Notify(cs, syscall::SIGINT_);

    constexpr int execs = 10;
    for (int i = 0; i < execs; i++) {
        auto [out, err] = helper_command("atomic_stop")->CombinedOutput();
        std::string_view o = as_string_view(out);
        bool lost = o.find("lost signal") != std::string_view::npos;
        EXPECT_FALSE(lost) << "iteration " << i << ": lost signal";
        // The program either dies of SIGINT, or exits with success without
        // printing "lost signal".
        if (!err) {
            EXPECT_TRUE(o.empty() || lost) << "iteration " << i << ": unexpected output " << o;
        } else {
            auto* ee = errors::As<os::exec::ExitError>(err);
            if (!ee) {
                ADD_FAILURE() << "iteration " << i << ": error (" << err << ") is not an exec::ExitError";
            } else {
                syscall::WaitStatus ws = ee->Sys();
                EXPECT_TRUE(ws.Signaled() && ws.Signal() == syscall::SIGINT_)
                    << "iteration " << i << ": got exit status " << err << "; expected SIGINT";
            }
        }
    }
    os::signal::Stop(cs);
}

TEST(GoOsSignal, Time) {
    // Signals work during calls that get the time (VDSO calls, Go's
    // issue 34391). Go runs 3s, or 100ms in short mode.
    const time::Duration dur = 300 * time::Millisecond;
    auto sig = make_chan<os::Signal>(1);
    os::signal::Notify(sig, syscall::SIGUSR1_);

    auto stop = make_chan<empty>();
    go([sig, stop] {
        for (;;) {
            bool stopped = false;
            select(recv(stop, [&] { stopped = true; }), default_case([] {
                       kill_self(syscall::SIGUSR1_);
                       runtime::Gosched();
                   }));
            if (stopped) {
                quiesce();
                os::signal::Stop(sig);
                sig.close();
                return;
            }
        }
    });

    auto done = make_chan<empty>();
    go([sig, done] {
        for (auto s : sig) (void)s;
        done.close();
    });

    auto t0 = time::Now();
    for (auto t1 = t0; t1.Sub(t0) < dur; t1 = time::Now()) {
    }  // hammering on getting time

    stop.close();
    done.recv();
}

TEST(GoOsSignal, NotifyContextNotifications) {
    for (int n : {1, 10}) {
        SCOPED_TRACE(n == 1 ? "once" : "multiple");
        auto [out, err] = helper_command("notify_ctx", {std::to_string(n)})->CombinedOutput();
        EXPECT_FALSE(err) << "ran test with -check_notify_ctx_notification and it failed with " << err
                          << ".\nOutput:\n"
                          << as_string_view(out);
        EXPECT_NE(as_string_view(out).find("received SIGINT\n"), std::string_view::npos)
            << "got " << as_string_view(out) << ", wanted received SIGINT";
    }
}

TEST(GoOsSignal, NotifyContextStop) {
    os::signal::Ignore(syscall::SIGHUP_);
    EXPECT_TRUE(os::signal::Ignored(syscall::SIGHUP_)) << "expected SIGHUP to be ignored when explicitly ignoring it.";

    auto [parent, cancel_parent] = context::WithCancel(context::Background());
    auto [c, stop] = os::signal::NotifyContext(parent, syscall::SIGHUP_);

    // If we're being notified, then the signal should not be ignored.
    EXPECT_FALSE(os::signal::Ignored(syscall::SIGHUP_)) << "expected SIGHUP to not be ignored.";

    stop();
    c->Done().recv();
    EXPECT_EQ(c->Err(), context::Canceled);
    EXPECT_EQ(context::Cause(c), context::Canceled);
    stop();
    cancel_parent();
    // Go leaves SIGHUP ignored; give it back its default.
    os::signal::Reset(syscall::SIGHUP_);
}

TEST(GoOsSignal, NotifyContextCancelParent) {
    auto [parent, cancel_parent] = context::WithCancelCause(context::Background());
    error parent_cause = errors::New("parent canceled");
    auto [c, stop] = os::signal::NotifyContext(parent, syscall::SIGINT_);

    cancel_parent(parent_cause);
    c->Done().recv();
    EXPECT_EQ(c->Err(), context::Canceled);
    EXPECT_EQ(context::Cause(c), parent_cause);
    stop();
}

TEST(GoOsSignal, NotifyContextPrematureCancelParent) {
    auto [parent, cancel_parent] = context::WithCancelCause(context::Background());
    error parent_cause = errors::New("parent canceled");

    cancel_parent(parent_cause);  // Prematurely cancel context before calling NotifyContext.
    auto [c, stop] = os::signal::NotifyContext(parent, syscall::SIGINT_);

    c->Done().recv();
    EXPECT_EQ(c->Err(), context::Canceled);
    EXPECT_EQ(context::Cause(c), parent_cause);
    stop();
}

TEST(GoOsSignal, NotifyContextSimultaneousStop) {
    auto [c, stop] = os::signal::NotifyContext(context::Background(), syscall::SIGINT_);
    sync::WaitGroup wg;
    constexpr int n = 10;
    wg.Add(n);
    for (int i = 0; i < n; i++) {
        go([&] {
            stop();
            wg.Done();
        });
    }
    wg.Wait();
    c->Done().recv();
    EXPECT_EQ(c->Err(), context::Canceled);
}

TEST(GoOsSignal, NotifyContextStringer) { GTEST_SKIP() << "go++'s contexts have no String"; }

TEST(GoOsSignal, NotifyContextCause) {
    auto [ctx, stop] = os::signal::NotifyContext(context::Background(), syscall::SIGINT_);
    kill_self(syscall::SIGINT_);
    ctx->Done().recv();

    error err = ctx->Err();
    EXPECT_TRUE(err) << "ctx.Err returned nil";
    EXPECT_TRUE(errors::Is(err, context::Canceled)) << "error " << err << " is not context.Canceled";
    err = context::Cause(ctx);
    EXPECT_TRUE(err) << "context.Cause returned nil";
    EXPECT_TRUE(errors::Is(err, context::Canceled)) << "cause " << err << " is not context.Canceled";
    EXPECT_EQ(err.Error(), "interrupt signal received");
    stop();
}

// go++'s own: os::Signal values, and SIGPIPE with Notify.

TEST(GoOsSignal, SignalValues) {
    os::Signal nil;
    EXPECT_TRUE(nil.is_nil());
    EXPECT_EQ(nil.String(), "<nil>");
    EXPECT_EQ(os::Interrupt, syscall::SIGINT_);
    EXPECT_EQ(os::Kill.String(), "killed");
    EXPECT_FALSE(os::Interrupt == os::Kill);
    EXPECT_EQ(syscall::SIGTERM_.String(), "terminated");
    EXPECT_EQ(syscall::Signal(99).String(), "signal 99");
    EXPECT_EQ(fmt::Sprint("{}", os::Signal(syscall::SIGHUP_)), "hangup");
    auto [s, ok] = os::Interrupt.as_syscall();
    EXPECT_TRUE(ok);
    EXPECT_EQ(s, syscall::SIGINT_);

    // A signal type of the program's own: not a syscall::Signal, so
    // Notify and Process::Signal don't take it.
    struct my_signal final : os::signal_interface {
        string String() const override { return "mine"; }
        void Signal() const override {}
    };
    os::Signal mine(make_ptr<my_signal>());
    EXPECT_EQ(mine.String(), "mine");
    EXPECT_FALSE(mine.as_syscall().second);
    EXPECT_FALSE(mine == os::Interrupt);
    EXPECT_TRUE(mine == mine);
    EXPECT_FALSE(os::signal::Ignored(mine));
    auto [p, err] = os::FindProcess(os::Getpid());
    ASSERT_FALSE(err);
    EXPECT_EQ(p->Signal(mine).Error(), "os: unsupported signal type");
}

TEST(GoOsSignal, NotifySIGPIPE) {
    // With SIGPIPE notified, a write to a broken pipe still fails with
    // EPIPE, and the channel gets the signal.
    auto c = make_chan<os::Signal>(1);
    os::signal::Notify(c, syscall::SIGPIPE_);
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err);
    r->Close();
    auto [_, werr] = w->Write(b("x"));
    EXPECT_TRUE(errors::Is(werr, syscall::EPIPE_)) << werr;
    wait_sig(c, syscall::SIGPIPE_);
    os::signal::Stop(c);
    // Without it, writes still fail with EPIPE, and the program lives on.
    auto [_2, werr2] = w->Write(b("x"));
    EXPECT_TRUE(errors::Is(werr2, syscall::EPIPE_)) << werr2;
    w->Close();
}
