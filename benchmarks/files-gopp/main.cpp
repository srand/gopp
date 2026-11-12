// File system calls from many fibers: c workers each repeat an operation
// on a set of small files for a duration, and the program prints the
// operations per second. Run it with GOPP_IOURING=0 and without to compare
// blocking calls with io_uring.
//
//     files -op read -c 64 -d 3s
//
// Operations: stat (os::Stat), read (os::ReadFile), open (Open and Close),
// write (WriteFile), readdir (os::ReadDir of the directory).
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <string>
#include <vector>

using namespace gopp;

int main(int argc, char** argv) {
    string op = "read", dur = "3s";
    int64 conns = 64, nfiles = 100, size = 4096;
    flag::StringVar(&op, "op", op, "operation: stat, read, open, write or readdir");
    flag::Int64Var(&conns, "c", conns, "concurrent workers");
    flag::StringVar(&dur, "d", dur, "duration");
    flag::Int64Var(&nfiles, "files", nfiles, "files to work on");
    flag::Int64Var(&size, "size", size, "file size in bytes");
    flag::Parse(argc, argv);
    auto [d, derr] = time::ParseDuration(dur.view());
    if (derr) {
        fmt::Println(os::Stderr, "{}", derr);
        return 2;
    }

    auto [dir, err] = os::MkdirTemp("", "gopp-files-bench");
    if (err) {
        fmt::Println(os::Stderr, "{}", err);
        return 1;
    }
    std::string data(static_cast<std::size_t>(size), 'x');
    std::vector<std::string> names;
    for (int64 i = 0; i < nfiles; i++) {
        names.push_back(std::string(dir.view()) + "/f" + std::to_string(i));
        if (error err = os::WriteFile(names.back(), data, 0644)) {
            fmt::Println(os::Stderr, "{}", err);
            return 1;
        }
    }

    std::atomic<int64> ops{0}, errs{0};
    std::atomic<bool> stop{false};
    sync::WaitGroup wg;
    for (int64 w = 0; w < conns; w++) {
        wg.Go([&, w] {
            int64 n = 0;
            for (std::size_t i = static_cast<std::size_t>(w); !stop.load(std::memory_order_relaxed); i++) {
                const std::string& name = names[i % names.size()];
                error e;
                if (op == "stat") {
                    e = os::Stat(name).second;
                } else if (op == "read") {
                    e = os::ReadFile(name).second;
                } else if (op == "open") {
                    auto [f, oerr] = os::Open(name);
                    e = oerr ? oerr : f->Close();
                } else if (op == "write") {
                    e = os::WriteFile(name, data, 0644);
                } else if (op == "readdir") {
                    e = os::ReadDir(dir.view()).second;
                } else {
                    os::Exit(2);
                }
                if (e) errs.fetch_add(1, std::memory_order_relaxed);
                n++;
            }
            ops.fetch_add(n, std::memory_order_relaxed);
        });
    }
    time::Sleep(d);
    stop.store(true);
    wg.Wait();
    os::RemoveAll(dir.view());
    double secs = d.Seconds();
    fmt::Println("{{\"op\": \"{}\", \"c\": {}, \"ops_per_sec\": {:.0f}, \"errors\": {}}}", op, conns,
                 static_cast<double>(ops.load()) / secs, errs.load());
    return 0;
}
