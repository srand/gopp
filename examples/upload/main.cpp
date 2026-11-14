// upload: a page to upload files to, and download them from. The page is
// an html/template, so the names and notes that users upload are escaped
// for wherever they land: HTML text, a link's URL, an attribute, a script.
//
//     $ upload [-addr :8080] [-dir uploads] [-max 10485760]
//     $ curl -F file=@notes.txt -F 'note=<b>my notes</b>' localhost:8080/upload
//     $ curl localhost:8080/
//     ...
//     <tr><td><a href="/files/notes.txt">notes.txt</a></td><td>1.2 KB</td><td>&lt;b&gt;my notes&lt;/b&gt;</td></tr>
//     ...
//     <script>const files = ["notes.txt"];</script>
//     $ curl localhost:8080/files/notes.txt
//
// The request body is a multipart/form-data form: ParseMultipartForm keeps
// up to 1 MB of it in memory and the rest in a temporary file, which the
// server removes after the handler. http::MaxBytesReader caps the body at
// -max bytes, and the files are written and served through an os::Root,
// so no name can reach outside -dir.
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/html/template.hpp>
#include <gopp/io.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>
#include <gopp/sync.hpp>

#include <map>
#include <vector>

using namespace gopp;
namespace htmpl = html::tmpl;

const char* pageTemplate = R"html(<!doctype html>
<title>Uploads</title>
<h1>Uploads</h1>
{{with .Error}}<p style="color: red">{{.}}</p>{{end}}
<form method="post" action="/upload" enctype="multipart/form-data">
  <input type="file" name="file" required>
  <input name="note" placeholder="A note about the file">
  <button>Upload</button>
</form>
<table>
{{range .Files}}<tr><td><a href="/files/{{.Name}}">{{.Name}}</a></td><td>{{.Size}}</td><td>{{.Note}}</td></tr>
{{else}}<tr><td>No files yet.</td></tr>
{{end}}</table>
<script>const files = {{.Names}};</script>
)html";

struct FileRow {
    string Name;
    string Size;
    string Note;
};

struct Page {
    string Error;
    std::vector<FileRow> Files;
    std::vector<string> Names;
};

string humanSize(int64 n) {
    if (n < 1000) return fmt::Sprint("{} B", n);
    if (n < 1000'000) return fmt::Sprint("{:.1f} KB", double(n) / 1e3);
    return fmt::Sprint("{:.1f} MB", double(n) / 1e6);
}

class Uploads {
public:
    Uploads(ptr<os::Root> root, int64 maxBytes) : root_(std::move(root)), maxBytes_(maxBytes) {}

    void Routes(http::ServeMux& mux) {
        mux.HandleFunc("GET /{$}", [this](http::ResponseWriter& w, http::Request& r) { show(w, string(), http::StatusOK); });
        mux.HandleFunc("POST /upload", [this](http::ResponseWriter& w, http::Request& r) { upload(w, r); });
        mux.Handle("GET /files/", http::StripPrefix("/files", http::FileServerFS(root_->FS())));
    }

private:
    // The page: the files in the directory, with the notes they came with.
    void show(http::ResponseWriter& w, string problem, int status) {
        Page page;
        page.Error = problem;
        auto [entries, err] = fs::ReadDir(*root_->FS(), ".");
        if (err) page.Error = err.Error();
        mu_.Lock();
        for (const auto& e : entries) {
            if (!e->Type().IsRegular()) continue;
            auto [info, ierr] = e->Info();
            string name = e->Name();
            page.Files.push_back({name, ierr ? string("?") : humanSize(info->Size()), notes_[name]});
            page.Names.push_back(name);
        }
        mu_.Unlock();
        w.Header().Set("Content-Type", "text/html; charset=utf-8");
        w.WriteHeader(status);
        if (auto err = page_->Execute(w, page)) fmt::Println(os::Stderr, "upload: {}", err);
    }

    void upload(http::ResponseWriter& w, http::Request& r) {
        r.Body = http::MaxBytesReader(&w, r.Body, maxBytes_);
        if (auto err = r.ParseMultipartForm(1 << 20)) {
            if (errors::As<http::MaxBytesError>(err)) {
                return show(w, fmt::Sprint("The file is larger than {}.", humanSize(maxBytes_)),
                            http::StatusRequestEntityTooLarge);
            }
            return show(w, err.Error(), http::StatusBadRequest);
        }
        auto [file, header, err] = r.FormFile("file");
        if (err) return show(w, "Choose a file to upload.", http::StatusBadRequest);

        // Browsers send the base name; others may send anything.
        string name = path::Base(header->Filename);
        if (name == "." || name == "/" || name.starts_with(".")) {
            return show(w, "That file name isn't allowed.", http::StatusBadRequest);
        }
        auto [dst, cerr] = root_->Create(name);
        if (cerr) return show(w, cerr.Error(), http::StatusInternalServerError);
        auto [n, copyErr] = io::Copy(*dst, *file);
        error closeErr = dst->Close();
        if (error e = copyErr ? copyErr : closeErr) return show(w, e.Error(), http::StatusInternalServerError);

        mu_.Lock();
        notes_[name] = r.FormValue("note");
        mu_.Unlock();
        fmt::Println("{} uploaded {} ({})", r.RemoteAddr, name, humanSize(n));
        // After a POST, send the browser back to the page with a GET.
        http::Redirect(w, r, "/", http::StatusSeeOther);
    }

    ptr<os::Root> root_;
    int64 maxBytes_;
    ptr<htmpl::Template> page_ = htmpl::Must(htmpl::New("page")->Parse(pageTemplate));
    sync::Mutex mu_;
    std::map<string, string, std::less<>> notes_;  // guarded by mu_
};

int main(int argc, char** argv) {
    string addr = ":8080", dir = "uploads";
    int64 maxBytes = 10 << 20;
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::StringVar(&dir, "dir", dir, "the directory to keep the files in");
    flag::Int64Var(&maxBytes, "max", maxBytes, "the largest upload, in bytes");
    flag::Parse(argc, argv);

    if (auto err = os::MkdirAll(dir, 0755)) {
        fmt::Println(os::Stderr, "upload: {}", err);
        return 1;
    }
    auto [root, err] = os::OpenRoot(dir);
    if (err) {
        fmt::Println(os::Stderr, "upload: {}", err);
        return 1;
    }

    Uploads uploads(root, maxBytes);
    auto mux = http::NewServeMux();
    uploads.Routes(*mux);
    fmt::Println("upload: http://localhost{}, files in {}", addr, root->Name());
    if (auto err = http::ListenAndServe(addr, mux)) {
        fmt::Println(os::Stderr, "upload: {}", err);
        return 1;
    }
}
