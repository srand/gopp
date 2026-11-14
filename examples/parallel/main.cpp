// parallel: runs shell commands concurrently, a few at a time, like
// xargs -P. Each line of standard input is a command for sh -c, and the
// lines of its output are printed as they come, after its job number.
//
//     $ printf 'sleep 1; echo one\nsleep 2; echo two\nexit 3\n' | parallel -j 2
//     [1] one
//     [3] exit status 3 (2ms)
//     [2] two
//     3 jobs, 1 failed in 2.01s
//
//     $ ls *.png | sed 's/.*/optipng -quiet &/' | parallel -j 8 -timeout 1m
//
// Ctrl-C, or a job's -timeout, cancels the jobs' contexts, and
// exec::CommandContext kills their processes.
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace gopp;
namespace exec = os::exec;
namespace signal = os::signal;

// Standard output, shared by the jobs: one line at a time.
sync::Mutex stdout_mu;

void printLine(std::string_view line) {
    std::lock_guard lock(stdout_mu);
    io::WriteString(os::Stdout, line);
}

// A Writer that prints whole lines with a prefix. The command's standard
// output and error both go to it, through pipes that exec copies from on
// fibers of its own.
class prefix_writer final : public io::Writer {
public:
    explicit prefix_writer(std::string prefix) : prefix_(std::move(prefix)) {}

    std::pair<isize, error> Write(std::span<const byte> p) override {
        buf_.append(reinterpret_cast<const char*>(p.data()), p.size());
        std::size_t start = 0;
        for (std::size_t nl; (nl = buf_.find('\n', start)) != buf_.npos; start = nl + 1) {
            printLine(prefix_ + buf_.substr(start, nl + 1 - start));
        }
        buf_.erase(0, start);
        return {static_cast<isize>(p.size()), nullptr};
    }

    // Prints a last line without a newline.
    void Flush() {
        if (!buf_.empty()) printLine(prefix_ + buf_ + "\n");
        buf_.clear();
    }

private:
    std::string prefix_;
    std::string buf_;
};

struct job {
    int n;
    string command;
};

int main(int argc, char** argv) {
    isize jobs = 4;
    time::Duration timeout{};
    flag::IntVar(&jobs, "j", jobs, "the commands run at once");
    flag::DurationVar(&timeout, "timeout", timeout, "the time limit of each command; 0 for none");
    flag::Parse(argc, argv);

    auto [input, err] = io::ReadAll(os::Stdin);
    if (err) {
        fmt::Println(os::Stderr, "parallel: {}", err);
        return 1;
    }
    std::vector<job> todo;
    for (const string& line : strings::Split(String(input), "\n")) {
        if (!strings::TrimSpace(line).empty()) todo.push_back({static_cast<int>(todo.size()) + 1, line});
    }

    // Ctrl-C cancels ctx, and with it every command.
    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);

    auto queue = makeChan<job>(todo.size());
    for (job& j : todo) queue.send(std::move(j));
    queue.close();

    std::atomic<int> failed{0};
    time::Time start = time::Now();
    sync::WaitGroup workers;
    for (isize w = 0; w < jobs; w++) {
        workers.Go([&] {
            for (const job& j : queue) {
                if (ctx->Err()) break;
                auto job_ctx = ctx;
                context::CancelFunc cancel = [] {};
                if (timeout.ns > 0) std::tie(job_ctx, cancel) = context::WithTimeout(ctx, timeout);

                auto out = make_ptr<prefix_writer>(fmt::Sprint("[{}] ", j.n).str());
                auto cmd = exec::CommandContext(job_ctx, "sh", "-c", j.command);
                cmd->Stdout = out;
                cmd->Stderr = out;
                // A killed shell may leave children holding the pipes:
                // don't wait for them for long.
                cmd->WaitDelay = time::Second;
                time::Time job_start = time::Now();
                error run_err = cmd->Run();
                out->Flush();
                cancel();

                if (run_err) {
                    failed++;
                    string why = run_err.Error();
                    if (job_ctx->Err() == context::DeadlineExceeded) why = string::fromStatic("timed out");
                    else if (auto* exit = errors::As<exec::ExitError>(run_err)) why = exit->String();
                    printLine(fmt::Sprint("[{}] {} ({})\n", j.n, why, time::Since(job_start)).view());
                }
            }
        });
    }
    workers.Wait();
    bool interrupted = ctx->Err() != nullptr;
    stop();  // which cancels ctx too

    fmt::Println("{} jobs, {} failed in {}{}", todo.size(), failed.load(), time::Since(start),
                 interrupted ? " (interrupted)" : "");
    return failed > 0 || interrupted ? 1 : 0;
}
