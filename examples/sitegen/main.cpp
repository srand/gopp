// sitegen: a small static site generator, written against vfs::Fs so that
// the same code builds to disk, to memory, or to memory over the disk.
//
//     $ sitegen -out public                 # the sample site, into ./public
//     create about.html
//     create index.html
//     ...
//     $ sitegen -out public                 # again: nothing has changed
//     0 files changed, 5 unchanged
//     $ sitegen -src mysite -out public -n  # what would change; writes nothing
//     $ sitegen -src mysite -serve :8080    # serves it from memory
//
// A site is a directory of pages, .txt files whose first line is the
// title and whose paragraphs are separated by blank lines, and of other
// files, such as style sheets, which are copied as they are. Each page
// becomes an .html file, and index.html lists them. Without -src, the
// sample site is built: it lives in a vfs::MemoryFs.
//
// The build reads a vfs::Fs and writes another, and the layers decide
// where things go:
//
//   - The sources are a ReadOnlyFs over a BasePathFs of the OsFs: the
//     build sees the site's directory as its root, and can't write to it.
//   - The output is a CopyOnWriteFs: the build reads the output directory
//     but writes to a MemoryFs over it. The files in the MemoryFs are then
//     compared with the disk's, and only those that differ are written, so
//     unchanged files keep their times. -n stops before writing. Files the
//     site no longer has are left where they are.
//   - -serve builds into a fresh MemoryFs on each request, and serves it
//     with http::FileServerFS through vfs::IOFS. Edits show on reload, and
//     nothing touches the disk.
//
// A MemoryFs keeps "a.txt" and "/a.txt" apart, and fs::FS names, which
// IOFS passes on, are relative, so the build uses relative names.
#include <gopp/bytes.hpp>
#include <gopp/cmp.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/html/template.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/slices.hpp>
#include <gopp/strings.hpp>
#include <gopp/vfs.hpp>

#include <vector>

using namespace gopp;
namespace htmpl = html::tmpl;

// What the layout renders: a page's title and paragraphs, and on the
// index, links to the pages.
struct Link {
    string Href;
    string Title;
};

struct View {
    string Title;
    std::vector<string> Paragraphs;
    std::vector<Link> Pages;
};

const char* layoutTemplate = R"(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>{{.Title}}</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
<h1>{{.Title}}</h1>
{{range .Paragraphs}}<p>{{.}}</p>
{{end}}{{with .Pages}}<ul>
{{range .}}<li><a href="{{.Href}}">{{.Title}}</a></li>
{{end}}</ul>
{{end}}</body>
</html>
)";

ptr<htmpl::Template> layout = htmpl::Must(htmpl::New("layout")->Parse(layoutTemplate));

// A page: the first line is the title, the rest paragraphs.
View parsePage(const string& text) {
    auto [title, body, _] = strings::Cut(text, "\n");
    View v{.Title = strings::TrimSpace(title), .Paragraphs = {}, .Pages = {}};
    for (const string& para : strings::Split(body, "\n\n")) {
        string p = strings::Join(strings::Fields(para), " ");
        if (!p.empty()) v.Paragraphs.push_back(p);
    }
    return v;
}

error render(vfs::Fs& out, std::string_view name, const View& v) {
    bytes::Buffer buf;
    if (auto err = layout->Execute(buf, v)) return fmt::Errorf("render {}: {}", name, fmt::W(err));
    return vfs::WriteFile(out, name, buf.Bytes(), 0644);
}

// Builds the site in src into out.
error build(vfs::Fs& src, vfs::Fs& out) {
    std::vector<Link> pages;
    error err = vfs::Walk(src, ".", [&](std::string_view name, const ptr<fs::FileInfo>& info, error err) -> error {
        if (err) return err;
        if (info->IsDir()) return out.MkdirAll(name, 0755);
        auto [data, rerr] = vfs::ReadFile(src, name);
        if (rerr) return rerr;
        if (filepath::Ext(name) != ".txt") return vfs::WriteFile(out, name, data, 0644);

        string html = strings::TrimSuffix(name, ".txt") + ".html";
        if (html == "index.html") return fmt::Errorf("{}: index.html is the list of pages", name);
        View page = parsePage(String(data));
        pages.push_back({html, page.Title});
        return render(out, html, page);
    });
    if (err) return err;

    slices::SortFunc(pages, [](const Link& a, const Link& b) { return cmp::Compare(a.Title, b.Title); });
    return render(out, "index.html", {.Title = "Pages", .Paragraphs = {}, .Pages = pages});
}

// Writes the files of staged that differ from disk's to disk, unless
// dryRun, and says what it does.
error commit(vfs::Fs& staged, vfs::Fs& disk, bool dryRun) {
    int changed = 0, unchanged = 0;
    error err = vfs::Walk(staged, ".", [&](std::string_view name, const ptr<fs::FileInfo>& info, error err) -> error {
        if (err || info->IsDir()) return err;
        auto [data, rerr] = vfs::ReadFile(staged, name);
        if (rerr) return rerr;

        auto [old, oerr] = vfs::ReadFile(disk, name);
        if (!oerr && bytes::Equal(old, data)) {
            unchanged++;
            return nullptr;
        }
        if (oerr && !errors::Is(oerr, fs::ErrNotExist)) return oerr;
        fmt::Println("{} {}", oerr ? "create" : "update", name);
        changed++;
        if (dryRun) return nullptr;
        if (auto err = disk.MkdirAll(filepath::Dir(name), 0755)) return err;
        return vfs::WriteFile(disk, name, data, 0644);
    });
    if (err) return err;
    fmt::Println("{} files changed, {} unchanged{}", changed, unchanged, dryRun ? " (dry run: nothing written)" : "");
    return nullptr;
}

// The sample site, in memory.
ptr<vfs::Fs> sampleSite() {
    auto site = vfs::NewMemoryFs();
    auto add = [&](std::string_view name, std::string_view text) {
        if (auto err = vfs::WriteFile(*site, name, as_bytes(text), 0644)) panic(err->Error());
    };
    add("about.txt", R"(About this site

It was built by sitegen, a go++ example, from files that only ever
existed in memory.

Run sitegen with -src to build a site of your own.)");
    add("fibers.txt", R"(Fibers

go++ runs each goroutine on a fiber of its own, and a scheduler like Go's
spreads them over the machine's threads.)");
    add("guides/vfs.txt", R"(The vfs package

vfs is go++'s port of afero: file systems on disk and in memory, and layers
that combine them. <ReadOnlyFs>, <CopyOnWriteFs> & friends.)");
    add("style.css", "body { font-family: sans-serif; max-width: 40em; margin: 2em auto; }\n");
    return site;
}

int main(int argc, char** argv) {
    string srcDir, outDir = "public", addr;
    bool dryRun = false;
    flag::StringVar(&srcDir, "src", srcDir, "the site's `directory`; the sample site if empty");
    flag::StringVar(&outDir, "out", outDir, "the `directory` to build the site into");
    flag::BoolVar(&dryRun, "n", dryRun, "show what would change, but write nothing");
    flag::StringVar(&addr, "serve", addr, "serve the site at `address` instead of building it to disk");
    flag::Parse(argc, argv);

    auto osFs = vfs::NewOsFs();
    ptr<vfs::Fs> src = srcDir.empty() ? sampleSite() : vfs::NewReadOnlyFs(vfs::NewBasePathFs(osFs, srcDir));

    if (!addr.empty()) {
        auto site = make_ptr<http::HandlerFunc>([src](http::ResponseWriter& w, http::Request& r) {
            auto mem = vfs::NewMemoryFs();
            if (auto err = build(*src, *mem)) {
                http::Error(w, err->Error(), http::StatusInternalServerError);
                return;
            }
            http::FileServerFS(vfs::NewIOFS(mem))->ServeHTTP(w, r);
        });
        fmt::Println("serving at http://{}/", addr);
        if (auto err = http::ListenAndServe(addr, site)) {
            fmt::Println(os::Stderr, "sitegen: {}", err);
            return 1;
        }
        return 0;
    }

    ptr<vfs::Fs> disk = vfs::NewBasePathFs(osFs, outDir);
    ptr<vfs::Fs> staged = vfs::NewMemoryFs();
    ptr<vfs::Fs> out = vfs::NewCopyOnWriteFs(vfs::NewReadOnlyFs(disk), staged);
    if (auto err = build(*src, *out)) {
        fmt::Println(os::Stderr, "sitegen: {}", err);
        return 1;
    }
    if (auto err = commit(*staged, *disk, dryRun)) {
        fmt::Println(os::Stderr, "sitegen: {}", err);
        return 1;
    }
    return 0;
}
