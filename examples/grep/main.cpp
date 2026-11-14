// grep: searches a directory tree for lines that contain a string. One
// fiber walks the tree with fs::WalkDir over os::DirFS, a pool of fibers
// reads and searches the files (on io_uring where the kernel has it), and
// the errors are gathered with errors::Join.
//
//     $ grep -dir ~/src/go -ext .go -i 'goroutine'
//     $ grep -j 32 TODO
//
// Directories whose names start with a period, such as .git, are skipped,
// and so are files that look binary.
#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace gopp;

int main(int argc, char** argv) {
    string dir = ".", exts;
    bool ignore_case = false;
    isize workers = 8;
    flag::StringVar(&dir, "dir", dir, "the directory to search");
    flag::StringVar(&exts, "ext", "", "search only files with these extensions: .go,.cpp");
    flag::BoolVar(&ignore_case, "i", false, "ignore case");
    flag::IntVar(&workers, "j", workers, "the files searched at once");
    flag::Parse(argc, argv);
    if (flag::Args().size() != 1) {
        fmt::Println(os::Stderr, "usage: grep [-dir d] [-ext .a,.b] [-i] [-j n] text");
        return 2;
    }
    string pattern = ignore_case ? strings::ToLower(flag::Args()[0]) : flag::Args()[0];
    std::vector<string> want_exts;
    if (!exts.empty()) {
        for (const string& e : strings::Split(exts, ",")) want_exts.push_back(e);
    }

    ptr<fs::FS> fsys = os::DirFS(dir);
    auto paths = makeChan<string>(64);
    // A file's matches are printed together, so that files don't mix.
    auto output = makeChan<string>(64);
    std::atomic<int64> matches{0}, searched{0};

    sync::Mutex mu;
    std::vector<error> errs;  // guarded by mu
    auto fail = [&](error err) {
        std::lock_guard lock(mu);
        errs.push_back(std::move(err));
    };

    // The searchers.
    sync::WaitGroup searchers;
    for (isize i = 0; i < workers; i++) {
        searchers.Go([&] {
            for (const string& name : paths) {
                auto [data, err] = fs::ReadFile(*fsys, name);
                if (err) {
                    fail(err);
                    continue;
                }
                std::string_view text = asStringView(data);
                // A NUL in the first block: binary.
                if (text.substr(0, 512).find('\0') != text.npos) continue;
                searched++;

                std::string found;
                int line_no = 0;
                while (!text.empty()) {
                    auto nl = text.find('\n');
                    std::string_view line = text.substr(0, nl);
                    text = nl == text.npos ? std::string_view() : text.substr(nl + 1);
                    line_no++;
                    bool hit = ignore_case ? strings::Contains(strings::ToLower(line), pattern)
                                           : strings::Contains(line, pattern);
                    if (!hit) continue;
                    matches++;
                    found += fmt::Sprint("{}:{}: {}\n", path::Join(dir, name), line_no, line).view();
                }
                if (!found.empty()) output.send(string(found));
            }
        });
    }

    // The printer.
    auto printed = makeChan<empty>();
    go([output, printed] {
        for (const string& s : output) fmt::Print("{}", s);
        printed.close();
    });

    // The walker: sends the files to search.
    error walk_err = fs::WalkDir(*fsys, ".", [&](std::string_view name, const ptr<fs::DirEntry>& d, error err) -> error {
        if (err) {
            fail(err);
            return nullptr;  // go on with the rest of the tree
        }
        if (d->IsDir()) {
            if (name != "." && path::Base(name).starts_with(".")) return fs::SkipDir;
            return nullptr;
        }
        if (!d->Type().IsRegular()) return nullptr;
        if (!want_exts.empty() &&
            std::find(want_exts.begin(), want_exts.end(), path::Ext(name)) == want_exts.end()) {
            return nullptr;
        }
        paths.send(string(name));
        return nullptr;
    });
    if (walk_err) fail(walk_err);

    paths.close();
    searchers.Wait();
    output.close();
    printed.recv();

    fmt::Println(os::Stderr, "{} matches in {} files", matches.load(), searched.load());
    if (error err = errors::Join(errs)) {
        fmt::Println(os::Stderr, "grep: {}", err);
        return 2;
    }
    return matches > 0 ? 0 : 1;
}
