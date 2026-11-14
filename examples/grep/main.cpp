// grep: searches a directory tree for lines that match a regular
// expression (regexp, RE2's syntax). One fiber walks the tree with
// fs::WalkDir over os::DirFS, a pool of fibers reads and searches the files
// (on io_uring where the kernel has it), and the errors are gathered with
// errors::Join.
//
//     $ grep -dir ~/src/go -ext .go -i 'go(routine|func)'
//     $ grep -j 32 'TODO|FIXME'
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
#include <gopp/regexp.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

using namespace gopp;

// Searches one file: its matching lines, "path:line: text", their number,
// and a read error. A binary file (a NUL in its first block) counts -1.
std::tuple<string, int64, error> searchFile(fs::FS& fsys, const regexp::Regexp& re, std::string_view dir,
                                            const string& name) {
    auto [data, err] = fs::ReadFile(fsys, name);
    if (err) return {string(), -1, err};
    std::string_view text = asStringView(data);
    if (text.substr(0, 512).find('\0') != text.npos) return {string(), -1, nullptr};

    std::string found;
    int64 n = 0;
    for (int lineNo = 1; !text.empty(); lineNo++) {
        auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == text.npos ? std::string_view() : text.substr(nl + 1);
        if (!re.MatchString(line)) continue;
        n++;
        found += fmt::Sprint("{}:{}: {}\n", path::Join(dir, name), lineNo, line).view();
    }
    return {string(found), n, nullptr};
}

int main(int argc, char** argv) {
    string dir = ".", exts;
    bool ignoreCase = false;
    isize workers = 8;
    flag::StringVar(&dir, "dir", dir, "the directory to search");
    flag::StringVar(&exts, "ext", "", "search only files with these extensions: .go,.cpp");
    flag::BoolVar(&ignoreCase, "i", false, "ignore case");
    flag::IntVar(&workers, "j", workers, "the files searched at once");
    flag::Parse(argc, argv);
    if (flag::Args().size() != 1) {
        fmt::Println(os::Stderr, "usage: grep [-dir d] [-ext .a,.b] [-i] [-j n] regexp");
        return 2;
    }
    auto [re, rerr] = regexp::Compile(ignoreCase ? "(?i)" + flag::Args()[0] : flag::Args()[0]);
    if (rerr) {
        fmt::Println(os::Stderr, "grep: {}", rerr);
        return 2;
    }
    std::vector<string> wantExts;
    if (!exts.empty()) {
        for (const string& e : strings::Split(exts, ",")) wantExts.push_back(e);
    }

    ptr<fs::FS> fsys = os::DirFS(dir);
    auto paths = make_chan<string>(64);
    // A file's matches are printed together, so that files don't mix.
    auto output = make_chan<string>(64);
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
                auto [found, n, err] = searchFile(*fsys, *re, dir, name);
                if (err) fail(err);
                if (n < 0) continue;  // binary
                searched++;
                matches += n;
                if (!found.empty()) output.send(found);
            }
        });
    }

    // The printer.
    auto printed = make_chan<empty>();
    go([output, printed] {
        for (const string& s : output) fmt::Print("{}", s);
        printed.close();
    });

    // The walker: sends the files to search.
    error walkErr = fs::WalkDir(*fsys, ".", [&](std::string_view name, const ptr<fs::DirEntry>& d, error err) -> error {
        if (err) {
            fail(err);
            return nullptr;  // go on with the rest of the tree
        }
        if (d->IsDir()) {
            if (name != "." && path::Base(name).starts_with(".")) return fs::SkipDir;
            return nullptr;
        }
        if (!d->Type().IsRegular()) return nullptr;
        if (!wantExts.empty() &&
            std::find(wantExts.begin(), wantExts.end(), path::Ext(name)) == wantExts.end()) {
            return nullptr;
        }
        paths.send(string(name));
        return nullptr;
    });
    if (walkErr) fail(walkErr);

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
