// os/exec, after Go's os/exec/exec.go, exec_unix.go and lp_unix.go.
#include "../internal.hpp"
#include "internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_set>

namespace gopp::os::exec {

const error ErrNotFound = errors::New("executable file not found in $PATH");
const error ErrDot = errors::New("cannot run executable found relative to current directory");
const error ErrWaitDelay = errors::New("exec: WaitDelay expired before I/O complete");

string Error::message() const { return "exec: " + strconv::Quote(Name) + ": " + Err.Error(); }

namespace {

// Go's wrappedError: "prefix: err", unwrapping to err.
struct wrappedError final : errorInterface {
    wrappedError(string prefix, gopp::error err) : prefix(std::move(prefix)), err(std::move(err)) {}
    string Error() const override { return prefix + ": " + err.Error(); }
    const errorInterface* Unwrap() const noexcept override { return err.get(); }
    string prefix;
    gopp::error err;
};

// Go's bytes.Buffer, as far as Output needs it.
class buffer final : public io::Writer {
public:
    std::pair<isize, error> Write(std::span<const byte> p) override {
        std::lock_guard l(mu_);
        b_.insert(b_.end(), p.begin(), p.end());
        return {static_cast<isize>(p.size()), nullptr};
    }
    slice<byte> Bytes() {
        std::lock_guard l(mu_);
        return slice<byte>::of(b_);
    }

private:
    sync::Mutex mu_;
    std::vector<byte> b_;
};

// Go's prefixSuffixSaver: keeps the first and last N bytes written.
class prefixSuffixSaver final : public io::Writer {
public:
    explicit prefixSuffixSaver(std::size_t n) : n_(n) {}

    std::pair<isize, error> Write(std::span<const byte> p) override {
        std::lock_guard l(mu_);
        isize lenp = static_cast<isize>(p.size());
        p = fill(prefix_, p);
        // Only the last n bytes of p can end up in the suffix.
        if (p.size() > n_) {
            std::size_t overage = p.size() - n_;
            p = p.subspan(overage);
            skipped_ += static_cast<int64>(overage);
        }
        p = fill(suffix_, p);
        // The suffix is full: overwrite it as a ring buffer.
        while (!p.empty()) {
            std::size_t n = std::min(p.size(), suffix_.size() - suffixOff_);
            std::copy_n(p.begin(), n, suffix_.begin() + static_cast<std::ptrdiff_t>(suffixOff_));
            p = p.subspan(n);
            skipped_ += static_cast<int64>(n);
            suffixOff_ += n;
            if (suffixOff_ == n_) suffixOff_ = 0;
        }
        return {lenp, nullptr};
    }

    slice<byte> Bytes() {
        std::lock_guard l(mu_);
        std::vector<byte> out(prefix_);
        if (skipped_ == 0) {
            out.insert(out.end(), suffix_.begin(), suffix_.end());
            return slice<byte>::of(out);
        }
        std::string mid = "\n... omitting " + std::to_string(skipped_) + " bytes ...\n";
        out.insert(out.end(), mid.begin(), mid.end());
        out.insert(out.end(), suffix_.begin() + static_cast<std::ptrdiff_t>(suffixOff_), suffix_.end());
        out.insert(out.end(), suffix_.begin(), suffix_.begin() + static_cast<std::ptrdiff_t>(suffixOff_));
        return slice<byte>::of(out);
    }

private:
    std::span<const byte> fill(std::vector<byte>& dst, std::span<const byte> p) {
        if (dst.size() < n_) {
            std::size_t add = std::min(p.size(), n_ - dst.size());
            dst.insert(dst.end(), p.begin(), p.begin() + static_cast<std::ptrdiff_t>(add));
            p = p.subspan(add);
        }
        return p;
    }

    sync::Mutex mu_;
    std::size_t n_;
    std::vector<byte> prefix_;
    std::vector<byte> suffix_;
    std::size_t suffixOff_ = 0;
    int64 skipped_ = 0;
};

void closeDescriptors(std::vector<ptr<os::File>>& closers) {
    for (auto& f : closers) f->Close();
}

// Go's skipStdinCopyError: a write to the stdin pipe failed with EPIPE
// because the command exited without reading all of it.
bool skipStdinCopyError(const error& err) {
    auto* pe = dynamic_cast<const fs::PathError*>(err.get());
    return pe && pe->Op == "write" && pe->Path == "|1" && pe->Err == syscall::EPIPE_;
}

// Go's findExecutable.
error findExecutable(std::string_view file) {
    auto [d, err] = os::Stat(file);
    if (err) return err;
    FileMode m = d->Mode();
    if (m.IsDir()) return syscall::EISDIR_;
    std::string f(file);
    // eaccess(2) asks with the effective ids, as the kernel's exec does.
    if (::eaccess(f.c_str(), X_OK) == 0) return nullptr;
    int e = errno;
    if (e != ENOSYS && e != EPERM) return syscall::errnoError(e);
    if (uint32(m) & 0111) return nullptr;
    return fs::ErrPermission;
}

// Go's filepath.Abs.
std::pair<string, error> absPath(std::string_view path) {
    if (!path.empty() && path[0] == '/') return {string(detail::cleanPath(path)), nullptr};
    auto [wd, err] = os::Getwd();
    if (err) return {string(), err};
    return {string(detail::cleanPath(os::detail::joinPath(wd.view(), path))), nullptr};
}

// Go's GODEBUG execerrdot=0 turns off ErrDot.
bool execerrdot() {
    string gd = os::Getenv("GODEBUG");
    std::string_view rest = gd.view();
    bool on = true;
    for (;;) {
        std::size_t c = rest.find(',');
        std::string_view kv = rest.substr(0, c);
        if (kv == "execerrdot=0") on = false;
        else if (kv.starts_with("execerrdot=")) on = true;
        if (c == std::string_view::npos) return on;
        rest = rest.substr(c + 1);
    }
}

}  // namespace

namespace detail {

std::pair<slice<string>, error> dedupEnvCase(bool caseInsensitive, bool nulOk, const slice<string>& env) {
    // Building backward makes the last value of a key win; the result is
    // reversed at the end.
    error err;
    std::vector<string> out;
    std::unordered_set<std::string> saw;
    for (std::size_t n = env.size(); n > 0; n--) {
        const string& kv = env[n - 1];
        if (!nulOk && kv.find('\0') != string::npos) {
            err = errors::New("exec: environment variable contains NUL");
            continue;
        }
        std::size_t i = kv.find('=');
        if (i == 0) {
            // A key that starts with "=" (Windows has them).
            // Go's strings.Index(kv[1:], "=") + 1: 0 if there's no other.
            i = kv.view().substr(1).find('=');
            i = i == string::npos ? 0 : i + 1;
        }
        if (i == string::npos) {
            if (!kv.empty()) out.push_back(kv);  // invalid entries are kept
            continue;
        }
        std::string k(kv.view().substr(0, i));
        if (caseInsensitive) k = strings::ToLower(k).str();
        if (!saw.insert(k).second) continue;
        out.push_back(kv);
    }
    std::reverse(out.begin(), out.end());
    return {slice<string>::of(out), err};
}

std::string cleanPath(std::string_view path) {
    if (path.empty()) return ".";
    bool rooted = path[0] == '/';
    std::size_t n = path.size();
    std::string out;
    std::size_t r = 0, dotdot = 0;
    if (rooted) {
        out.push_back('/');
        r = dotdot = 1;
    }
    while (r < n) {
        if (path[r] == '/') {
            r++;  // empty path element
        } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
            r++;  // . element
        } else if (path[r] == '.' && r + 1 < n && path[r + 1] == '.' && (r + 2 == n || path[r + 2] == '/')) {
            // .. element: remove to the last /
            r += 2;
            if (out.size() > dotdot) {
                out.pop_back();
                while (out.size() > dotdot && out.back() != '/') out.pop_back();
                if (out.size() > dotdot && out.back() == '/') out.pop_back();
            } else if (!rooted) {
                // Can't backtrack: keep "..".
                if (!out.empty()) out.push_back('/');
                out += "..";
                dotdot = out.size();
            }
        } else {
            // A real element: add a slash if needed.
            if ((rooted && out.size() != 1) || (!rooted && !out.empty())) out.push_back('/');
            for (; r < n && path[r] != '/'; r++) out.push_back(path[r]);
        }
    }
    if (out.empty()) return ".";
    return out;
}

}  // namespace detail

namespace {

std::pair<slice<string>, error> dedupEnv(const slice<string>& env) { return detail::dedupEnvCase(false, false, env); }

}  // namespace

// ---------------------------------------------------------------------------
// LookPath

std::pair<string, error> LookPath(std::string_view file) {
    // Go's validateLookPath.
    if (file.empty() || file == "." || file == "..") return {string(), make_ptr<Error>(string(file), ErrNotFound)};
    if (file.find('/') != std::string_view::npos) {
        if (error err = findExecutable(file)) return {string(), make_ptr<Error>(string(file), err)};
        return {string(file), nullptr};
    }
    string path = os::Getenv("PATH");
    std::string_view rest = path.view();
    // Go's filepath.SplitList: an empty $PATH has no entries.
    while (!rest.empty()) {
        std::size_t c = rest.find(os::PathListSeparator);
        std::string_view dir = rest.substr(0, c);
        if (dir.empty()) dir = ".";  // Unix shell semantics: an empty entry is the working directory
        std::string p = detail::cleanPath(os::detail::joinPath(dir, file));
        if (!findExecutable(p)) {
            if (p[0] != '/' && execerrdot()) return {string(p), make_ptr<Error>(string(file), ErrDot)};
            return {string(p), nullptr};
        }
        if (c == std::string_view::npos) break;
        rest = rest.substr(c + 1);
    }
    return {string(), make_ptr<Error>(string(file), ErrNotFound)};
}

// ---------------------------------------------------------------------------
// Cmd

ptr<Cmd> Command(std::string_view name, std::span<const string> args) {
    auto cmd = make_ptr<Cmd>();
    cmd->Path = string(name);
    slice<string> a = slice<string>::make(0, args.size() + 1);
    a = append(a, string(name));
    for (const string& s : args) a = append(a, s);
    cmd->Args = a;
    // A bare name (filepath.Base(name) == name) is looked up in $PATH.
    if (name.find('/') == std::string_view::npos && !name.empty()) {
        auto [lp, err] = LookPath(name);
        if (!lp.empty()) cmd->Path = lp;
        if (err) cmd->Err = err;
    }
    return cmd;
}

ptr<Cmd> CommandContext(ptr<context::Context> ctx, std::string_view name, std::span<const string> args) {
    if (!ctx) panic("nil Context");
    ptr<Cmd> cmd = Command(name, args);
    cmd->ctx_ = std::move(ctx);
    // The default Cancel kills the process. It refers to cmd without a
    // reference, which would be a cycle: it's only called while cmd lives.
    Cmd* c = cmd.get();
    cmd->Cancel = [c]() -> error { return c->Process->Kill(); };
    return cmd;
}

Cmd::~Cmd() = default;

string Cmd::String() const {
    if (Err) return strings::Join(std::span<const string>(Args.begin(), Args.size()), " ");
    std::string b(Path.view());
    slice<string> av = argv();
    for (std::size_t i = 1; i < av.size(); i++) {
        b += ' ';
        b += av[i].view();
    }
    return string(b);
}

slice<string> Cmd::argv() const {
    if (Args.len() > 0) return Args;
    return slice<string>{Path};
}

std::pair<ptr<os::File>, error> Cmd::childStdin() {
    if (!Stdin) {
        auto [f, err] = os::Open(os::DevNull);
        if (err) return {nullptr, err};
        childIoFiles_.push_back(f);
        return {f, nullptr};
    }
    if (auto f = dynamic_pointer_cast<os::File>(Stdin)) return {f, nullptr};

    auto [pr, pw, err] = os::Pipe();
    if (err) return {nullptr, err};
    childIoFiles_.push_back(pr);
    parentIoPipes_.push_back(pw);
    goroutine_.push_back([pw, stdin = Stdin]() -> error {
        auto [_, err] = io::Copy(*pw, *stdin);
        if (skipStdinCopyError(err)) err = nullptr;
        if (error err1 = pw->Close(); !err) err = err1;
        return err;
    });
    return {pr, nullptr};
}

std::pair<ptr<os::File>, error> Cmd::childStdout() { return writerDescriptor(Stdout); }

std::pair<ptr<os::File>, error> Cmd::childStderr(const ptr<os::File>& childStdout) {
    // The same Writer for both: one descriptor, so that one fiber writes.
    if (Stderr && Stderr.get() == Stdout.get()) return {childStdout, nullptr};
    return writerDescriptor(Stderr);
}

std::pair<ptr<os::File>, error> Cmd::writerDescriptor(const ptr<io::Writer>& w) {
    if (!w) {
        auto [f, err] = os::OpenFile(os::DevNull, os::O_WRONLY_, 0);
        if (err) return {nullptr, err};
        childIoFiles_.push_back(f);
        return {f, nullptr};
    }
    if (auto f = dynamic_pointer_cast<os::File>(w)) return {f, nullptr};

    auto [pr, pw, err] = os::Pipe();
    if (err) return {nullptr, err};
    childIoFiles_.push_back(pw);
    parentIoPipes_.push_back(pr);
    goroutine_.push_back([w, pr]() -> error {
        auto [_, err] = io::Copy(*w, *pr);
        pr->Close();  // in case io::Copy stopped at a write error
        return err;
    });
    return {pw, nullptr};
}

error Cmd::Run() {
    if (error err = Start()) return err;
    return Wait();
}

error Cmd::Start() {
    // Start can be called only once.
    if (startCalled_.exchange(true)) return errors::New("exec: already started");

    bool started = false;
    struct cleanup {
        Cmd* c;
        bool& started;
        ~cleanup() {
            closeDescriptors(c->childIoFiles_);
            c->childIoFiles_.clear();
            if (!started) {
                closeDescriptors(c->parentIoPipes_);
                c->parentIoPipes_.clear();
                c->goroutine_.clear();
            }
        }
    } cleanup{this, started};

    if (Path.empty() && !Err) Err = errors::New("exec: no command");
    if (Err) return Err;
    if (Cancel && !ctx_) return errors::New("exec: command with a non-nil Cancel was not created with CommandContext");
    if (ctx_) {
        bool closed = false;
        empty e;
        if (chan<empty> done = ctx_->Done(); done && (done.tryRecv(e, &closed) || closed)) return ctx_->Err();
    }

    std::vector<ptr<os::File>> childFiles;
    childFiles.reserve(3 + ExtraFiles.size());
    auto [in, err] = childStdin();
    if (err) return err;
    childFiles.push_back(in);
    auto [out, oerr] = childStdout();
    if (oerr) return oerr;
    childFiles.push_back(out);
    auto [errf, eerr] = childStderr(out);
    if (eerr) return eerr;
    childFiles.push_back(errf);
    childFiles.insert(childFiles.end(), ExtraFiles.begin(), ExtraFiles.end());

    auto [env, enverr] = environ();
    if (enverr) return enverr;

    os::ProcAttr attr;
    attr.Dir = Dir;
    attr.Files = std::move(childFiles);
    attr.Env = env;
    attr.Sys = SysProcAttr;
    slice<string> av = argv();
    auto [p, perr] = os::StartProcess(Path, std::span<const string>(av.begin(), av.size()), attr);
    if (perr) return perr;
    Process = p;
    started = true;

    if (!goroutine_.empty()) {
        // The copying fibers; the first error goes to goroutineErr_ when
        // the last one is done.
        struct status {
            sync::Mutex mu;
            int running;
            error firstErr;
        };
        auto st = std::make_shared<status>();
        st->running = static_cast<int>(goroutine_.size());
        auto goroutineErr = make_chan<error>(1);
        goroutineErr_ = goroutineErr;
        for (auto& fn : goroutine_) {
            go([fn = std::move(fn), st, goroutineErr] {
                error err = fn();
                std::unique_lock l(st->mu);
                if (!st->firstErr) st->firstErr = err;
                if (--st->running == 0) {
                    error first = st->firstErr;
                    l.unlock();
                    goroutineErr.send(first);
                }
            });
        }
        goroutine_.clear();
    }

    if ((Cancel || WaitDelay.ns != 0) && ctx_ && ctx_->Done()) {
        auto resultc = make_chan<ctxResult>();
        ctxResult_ = resultc;
        go([self = ptr<Cmd>(this), resultc] { self->watchCtx(resultc); });
    }
    return nullptr;
}

// Go's watchCtx: runs Cancel if the context is done before Wait, then
// kills the process if it hasn't exited after WaitDelay. Sends the
// outcome to resultc, which Wait receives.
void Cmd::watchCtx(chan<ctxResult> resultc) {
    bool waited = false;
    select(send(resultc, ctxResult{}, [&] { waited = true; }), recv(ctx_->Done(), [] {}));
    if (waited) return;

    error err;
    if (Cancel) {
        if (error interruptErr = Cancel(); !interruptErr) {
            // The process was interrupted: report the context's error, so
            // that the caller knows why it didn't succeed.
            err = ctx_->Err();
        } else if (errors::Is(interruptErr, os::ErrProcessDone)) {
            // It had exited already: no error.
        } else {
            err = make_ptr<wrappedError>(string("exec: canceling Cmd"), interruptErr);
        }
    }
    if (WaitDelay.ns == 0) {
        resultc.send(ctxResult{err, nullptr});
        return;
    }

    ptr<time::Timer> timer = time::NewTimer(WaitDelay);
    bool sent = false;
    select(send(resultc, ctxResult{err, timer}, [&] { sent = true; }), recv(timer->C, [] {}));
    if (sent) return;

    bool killed = false;
    if (error killErr = Process->Kill(); !killErr) {
        killed = true;
    } else if (!errors::Is(killErr, os::ErrProcessDone)) {
        err = make_ptr<wrappedError>(string("exec: killing Cmd"), killErr);
    }

    if (goroutineErr_) {
        error gerr;
        bool got = false;
        select(recv(goroutineErr_, [&](error e) {
                   gerr = std::move(e);
                   got = true;
               }),
               defaultCase([] {}));
        if (got) {
            if (!err && !killed) err = gerr;
        } else {
            // Close the pipes, so that the copying stops even if a process
            // that inherited them still runs.
            closeDescriptors(parentIoPipes_);
            goroutineErr_.recv();
            if (!err) err = ErrWaitDelay;
        }
        // Wait has nothing more to wait for.
        goroutineErr_ = nullptr;
    }
    resultc.send(ctxResult{err, nullptr});
}

error Cmd::Wait() {
    if (!Process) return errors::New("exec: not started");
    if (ProcessState) return errors::New("exec: Wait was already called");

    auto [state, err] = Process->Wait();
    if (!err && !state->Success()) err = make_ptr<ExitError>(state);
    ProcessState = state;

    ptr<time::Timer> timer;
    if (ctxResult_) {
        ctxResult watch = ctxResult_.recv();
        timer = watch.timer;
        if (!err && watch.err) err = watch.err;
    }

    if (error goroutineErr = awaitGoroutines(timer); !err) err = goroutineErr;
    closeDescriptors(parentIoPipes_);
    parentIoPipes_.clear();
    return err;
}

// Go's awaitGoroutines: waits for the copying fibers, but no longer than
// WaitDelay after the timer started.
error Cmd::awaitGoroutines(ptr<time::Timer> timer) {
    struct cleanup {
        Cmd* c;
        ptr<time::Timer>& timer;
        ~cleanup() {
            if (timer) timer->Stop();
            c->goroutineErr_ = nullptr;
        }
    } cleanup{this, timer};

    if (!goroutineErr_) return nullptr;  // no fibers to wait for
    if (!timer) {
        if (WaitDelay.ns == 0) return goroutineErr_.recv();
        error err;
        bool got = false;
        select(recv(goroutineErr_, [&](error e) {
                   err = std::move(e);
                   got = true;
               }),
               defaultCase([] {}));
        if (got) return err;
        // The process exited, but its pipes are still open: give them
        // WaitDelay to close.
        timer = time::NewTimer(WaitDelay);
    }
    error err;
    bool expired = false;
    select(recv(timer->C, [&] { expired = true; }), recv(goroutineErr_, [&](error e) { err = std::move(e); }));
    if (expired) {
        closeDescriptors(parentIoPipes_);
        goroutineErr_.recv();
        return ErrWaitDelay;
    }
    return err;
}

std::pair<slice<byte>, error> Cmd::Output() {
    if (Stdout) return {slice<byte>(), errors::New("exec: Stdout already set")};
    auto out = make_ptr<buffer>();
    Stdout = out;

    ptr<prefixSuffixSaver> saver;
    if (!Stderr) {
        saver = make_ptr<prefixSuffixSaver>(32 << 10);
        Stderr = saver;
    }
    error err = Run();
    if (err && saver) {
        // The ExitError was made by Wait, not const.
        if (auto* ee = dynamic_cast<const ExitError*>(err.get())) const_cast<ExitError*>(ee)->Stderr = saver->Bytes();
    }
    return {out->Bytes(), err};
}

std::pair<slice<byte>, error> Cmd::CombinedOutput() {
    if (Stdout) return {slice<byte>(), errors::New("exec: Stdout already set")};
    if (Stderr) return {slice<byte>(), errors::New("exec: Stderr already set")};
    auto b = make_ptr<buffer>();
    Stdout = b;
    Stderr = b;
    error err = Run();
    return {b->Bytes(), err};
}

std::pair<ptr<os::File>, error> Cmd::StdinPipe() {
    if (Stdin) return {nullptr, errors::New("exec: Stdin already set")};
    if (Process) return {nullptr, errors::New("exec: StdinPipe after process started")};
    auto [pr, pw, err] = os::Pipe();
    if (err) return {nullptr, err};
    Stdin = pr;
    childIoFiles_.push_back(pr);
    parentIoPipes_.push_back(pw);
    return {pw, nullptr};
}

std::pair<ptr<os::File>, error> Cmd::StdoutPipe() {
    if (Stdout) return {nullptr, errors::New("exec: Stdout already set")};
    if (Process) return {nullptr, errors::New("exec: StdoutPipe after process started")};
    auto [pr, pw, err] = os::Pipe();
    if (err) return {nullptr, err};
    Stdout = pw;
    childIoFiles_.push_back(pw);
    parentIoPipes_.push_back(pr);
    return {pr, nullptr};
}

std::pair<ptr<os::File>, error> Cmd::StderrPipe() {
    if (Stderr) return {nullptr, errors::New("exec: Stderr already set")};
    if (Process) return {nullptr, errors::New("exec: StderrPipe after process started")};
    auto [pr, pw, err] = os::Pipe();
    if (err) return {nullptr, err};
    Stderr = pw;
    childIoFiles_.push_back(pw);
    parentIoPipes_.push_back(pr);
    return {pr, nullptr};
}

std::pair<slice<string>, error> Cmd::environ() const {
    error err;
    slice<string> env = Env;
    if (env.isNil()) {
        env = os::Environ();
        // The process's working directory: PWD, as a shell sets it, for
        // programs that look at it.
        if (!Dir.empty()) {
            auto [pwd, absErr] = absPath(Dir);
            if (!absErr) env = append(env, "PWD=" + pwd);
            else err = absErr;
        }
    }
    auto [deduped, dedupErr] = dedupEnv(env);
    if (!err) err = dedupErr;
    return {deduped, err};
}

slice<string> Cmd::Environ() const { return environ().first; }

}  // namespace gopp::os::exec
