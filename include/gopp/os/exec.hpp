// The os/exec package: runs external commands.
//
//     auto [out, err] = exec::Command("date", "+%F")->Output();
//
//     auto cmd = exec::CommandContext(ctx, "sort");
//     cmd->Stdin = strings::NewReader("b\na\n");
//     auto out = make_ptr<strings::Builder>();
//     cmd->Stdout = out;
//     if (auto err = cmd->Run()) ...
//
// Wait parks only its fiber (see os::Process). A Stdin, Stdout or Stderr
// that isn't an os::File is connected to the process by a pipe and a fiber
// that copies, as in Go.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::os::exec {

namespace detail {
// Implements Error() for an error type named Error, which can't declare it.
template <typename E>
struct named_error : errorInterface {
    string Error() const override { return static_cast<const E&>(*this).message(); }
};
}  // namespace detail

// exec.Error: LookPath couldn't find or run an executable. C++ reserves
// the name Error for its constructors: call Error() through gopp::error,
// or message().
struct Error : detail::named_error<Error> {
    Error(string name, gopp::error err) : Name(std::move(name)), Err(std::move(err)) {}
    string Name;
    gopp::error Err;
    // "exec: \"name\": err"
    string message() const;
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
};

// exec.ExitError: a command ran and didn't exit with status 0. Go's
// embedded *os.ProcessState's methods are forwarded.
struct ExitError : errorInterface {
    explicit ExitError(ptr<os::ProcessState> ps) : ProcessState(std::move(ps)) {}
    ptr<os::ProcessState> ProcessState;
    // Some of the standard error of the command, if Output collected it.
    slice<byte> Stderr;

    string Error() const override { return ProcessState->String(); }
    int ExitCode() const noexcept { return ProcessState->ExitCode(); }
    bool Exited() const noexcept { return ProcessState->Exited(); }
    bool Success() const noexcept { return ProcessState->Success(); }
    int Pid() const noexcept { return ProcessState->Pid(); }
    syscall::WaitStatus Sys() const noexcept { return ProcessState->Sys(); }
    const syscall::Rusage* SysUsage() const noexcept { return ProcessState->SysUsage(); }
    time::Duration UserTime() const noexcept { return ProcessState->UserTime(); }
    time::Duration SystemTime() const noexcept { return ProcessState->SystemTime(); }
    string String() const { return ProcessState->String(); }
};

// exec.ErrNotFound: "executable file not found in $PATH".
extern const error ErrNotFound;
// exec.ErrDot: LookPath found the executable relative to the working
// directory, through a relative entry of $PATH.
extern const error ErrDot;
// exec.ErrWaitDelay: "exec: WaitDelay expired before I/O complete".
extern const error ErrWaitDelay;

// exec.Cmd: a command to run. Make it with Command or CommandContext, set
// its fields, and Run it, or Start and Wait. A Cmd can't be reused.
class Cmd final : public object {
public:
    // The command's path; Command sets it to LookPath's result.
    string Path;
    // The arguments, with the command name first; empty means {Path}.
    slice<string> Args;
    // The environment ("key=value"); nil means this process's. Of keys
    // given twice, the last value counts.
    slice<string> Env;
    // The working directory; empty means this process's.
    string Dir;
    // Standard input: nil reads /dev/null. An os::File is given to the
    // process; another Reader is copied to a pipe by a fiber, which Wait
    // waits for.
    ptr<io::Reader> Stdin;
    // Standard output and error: nil writes to /dev/null. An os::File is
    // given to the process; for another Writer a fiber copies from a pipe.
    // The same Writer for both is written by one fiber at a time.
    ptr<io::Writer> Stdout;
    ptr<io::Writer> Stderr;
    // More files for the process, as descriptors 3, 4, ...; a nil entry
    // closes one.
    std::vector<ptr<os::File>> ExtraFiles;
    // Linux's process attributes.
    std::optional<syscall::SysProcAttr> SysProcAttr;
    // The process, once started.
    ptr<os::Process> Process;
    // How the process ended, once waited for. (Go's methods of a nil
    // *ProcessState, such as ExitCode, can't be called here: check the
    // ptr.)
    ptr<os::ProcessState> ProcessState;
    // An error of Command, such as LookPath's.
    gopp::error Err;
    // With CommandContext: called when the context is done before the
    // command exits. It kills the process by default; nil leaves it.
    std::function<gopp::error()> Cancel;
    // How long Wait waits, after the context is done or the process
    // exited, for the copying fibers before it closes the pipes, and after
    // Cancel before it kills the process. 0: forever.
    time::Duration WaitDelay;

    // Runs the command and waits for it. An error is an *ExitError if the
    // command ran but didn't succeed.
    gopp::error Run();
    // Starts the command; Wait must follow.
    gopp::error Start();
    // Waits for the command to exit and for its copying to finish, and
    // releases its resources.
    gopp::error Wait();
    // Runs the command and returns its standard output. If Stderr is nil,
    // an ExitError gets some of the standard error.
    std::pair<slice<byte>, gopp::error> Output();
    // Runs the command and returns its standard output and error.
    std::pair<slice<byte>, gopp::error> CombinedOutput();
    // Pipes to the standard input, from standard output and error, for
    // use between Start and Wait, which closes them. Go returns them as
    // io.WriteCloser and io.ReadCloser; os::File is both.
    std::pair<ptr<os::File>, gopp::error> StdinPipe();
    std::pair<ptr<os::File>, gopp::error> StdoutPipe();
    std::pair<ptr<os::File>, gopp::error> StderrPipe();
    // The command line, for display: path and arguments.
    string String() const;
    // The environment the command runs with.
    slice<string> Environ() const;

    ~Cmd() override;

    // Command and CommandContext make Cmds; Cmd{} works too, with Path set.
    Cmd() = default;

private:
    friend ptr<Cmd> CommandContext(ptr<context::Context> ctx, std::string_view name, std::span<const string> args);
    struct ctx_result {
        gopp::error err;
        ptr<time::Timer> timer;
    };
    std::pair<ptr<os::File>, gopp::error> childStdin();
    std::pair<ptr<os::File>, gopp::error> childStdout();
    std::pair<ptr<os::File>, gopp::error> childStderr(const ptr<os::File>& childStdout);
    std::pair<ptr<os::File>, gopp::error> writerDescriptor(const ptr<io::Writer>& w);
    std::pair<slice<string>, gopp::error> environ() const;
    slice<string> argv() const;
    void watch_ctx(chan<ctx_result> resultc);
    gopp::error await_goroutines(ptr<time::Timer> timer);

    ptr<context::Context> ctx_;
    std::vector<ptr<os::File>> child_io_files_;
    std::vector<ptr<os::File>> parent_io_pipes_;
    std::vector<std::function<gopp::error()>> goroutine_;
    chan<gopp::error> goroutine_err_;
    chan<ctx_result> ctx_result_;
    std::atomic<bool> start_called_{false};
};

// exec.Command: a Cmd that runs name with the arguments. A name without a
// slash is looked up in $PATH (LookPath); if that fails, Start returns the
// error.
ptr<Cmd> Command(std::string_view name, std::span<const string> args);
template <typename... A>
    requires(std::convertible_to<const A&, std::string_view> && ...)
ptr<Cmd> Command(std::string_view name, const A&... args) {
    const string a[] = {string(std::string_view(args))..., string()};
    return Command(name, std::span<const string>(a, sizeof...(A)));
}

// exec.CommandContext: like Command, but Cancel (killing the process, by
// default) is called if ctx is done before the command exits.
ptr<Cmd> CommandContext(ptr<context::Context> ctx, std::string_view name, std::span<const string> args);
template <typename... A>
    requires(std::convertible_to<const A&, std::string_view> && ...)
ptr<Cmd> CommandContext(ptr<context::Context> ctx, std::string_view name, const A&... args) {
    const string a[] = {string(std::string_view(args))..., string()};
    return CommandContext(std::move(ctx), name, std::span<const string>(a, sizeof...(A)));
}

// exec.LookPath: the path of the executable file, searched for in the
// directories of $PATH if it has no slash. An error is an *Error.
std::pair<string, gopp::error> LookPath(std::string_view file);

}  // namespace gopp::os::exec
