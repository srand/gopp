// After Go's net/http/fs.go: FileServer, ServeFile and ServeContent.
#include "internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/mime.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <array>
#include <random>

namespace gopp::http {

namespace {

// Dir.Open's error for names the system can't represent (a NUL byte).
const gopp::error errInvalidUnsafePath = errors::New("http: invalid or unsafe file path");
// ServeContent's size error: the Seeker's own error isn't sent to users.
const gopp::error errSeeker = errors::New("seeker can't seek");
// parseRange's error when no range starts within the content.
const gopp::error errNoOverlap = errors::New("invalid range: failed to overlap");
const gopp::error errMissingSeek = errors::New("io.File missing Seek method");
const gopp::error errMissingReadDir = errors::New("io.File directory missing ReadDir method");

// The fs.ReadDirFile side of a File: dirList prefers ReadDir to Readdir,
// which stats every entry. Dir's and FS's files have it.
struct readDirFile : public virtual object {
    virtual std::pair<std::vector<ptr<fs::DirEntry>>, gopp::error> ReadDir(int count) = 0;
};

using statFn = std::function<std::pair<ptr<fs::FileInfo>, gopp::error>(std::string_view)>;

// mapOpenError: turns the error of opening a file under a non-directory
// ("index.html/x": ENOTDIR) into fs::ErrNotExist.
gopp::error mapOpenError(gopp::error original, std::string_view name, const statFn& stat) {
    if (errors::Is(original, fs::ErrNotExist) || errors::Is(original, fs::ErrPermission)) return original;
    // Each prefix of name that ends a non-empty element, as Go joins the
    // elements split at the separator.
    for (std::size_t start = 0; start <= name.size();) {
        auto slash = name.find('/', start);
        std::size_t end = slash == name.npos ? name.size() : slash;
        if (end > start) {
            auto [fi, err] = stat(name.substr(0, end));
            if (err) return original;
            if (!fi->IsDir()) return fs::ErrNotExist;
        }
        if (slash == name.npos) break;
        start = slash + 1;
    }
    return original;
}

// An os::File as a File.
class dirFile final : public File, public readDirFile {
public:
    explicit dirFile(ptr<os::File> f) : f_(std::move(f)) {}
    std::pair<isize, gopp::error> Read(std::span<byte> p) override { return f_->Read(p); }
    std::pair<int64, gopp::error> Seek(int64 offset, int whence) override { return f_->Seek(offset, whence); }
    gopp::error Close() override { return f_->Close(); }
    std::pair<std::vector<ptr<fs::FileInfo>>, gopp::error> Readdir(int count) override { return f_->Readdir(count); }
    std::pair<ptr<fs::FileInfo>, gopp::error> Stat() override { return f_->Stat(); }
    std::pair<std::vector<ptr<fs::DirEntry>>, gopp::error> ReadDir(int count) override { return f_->ReadDir(count); }
    os::File* osFile() const noexcept { return f_.get(); }

private:
    ptr<os::File> f_;
};

// ioFile: an fs::File as a File.
class ioFile final : public File, public readDirFile {
public:
    explicit ioFile(ptr<fs::File> f) : file_(std::move(f)) {}
    gopp::error Close() override { return file_->Close(); }
    std::pair<isize, gopp::error> Read(std::span<byte> p) override { return file_->Read(p); }
    std::pair<ptr<fs::FileInfo>, gopp::error> Stat() override { return file_->Stat(); }

    std::pair<int64, gopp::error> Seek(int64 offset, int whence) override {
        auto* s = dynamic_cast<io::Seeker*>(file_.get());
        if (!s) return {0, errMissingSeek};
        return s->Seek(offset, whence);
    }

    std::pair<std::vector<ptr<fs::DirEntry>>, gopp::error> ReadDir(int count) override {
        auto* d = dynamic_cast<fs::ReadDirFile*>(file_.get());
        if (!d) return {{}, errMissingReadDir};
        return d->ReadDir(count);
    }

    std::pair<std::vector<ptr<fs::FileInfo>>, gopp::error> Readdir(int count) override {
        auto* d = dynamic_cast<fs::ReadDirFile*>(file_.get());
        if (!d) return {{}, errMissingReadDir};
        std::vector<ptr<fs::FileInfo>> list;
        for (;;) {
            auto [dirs, err] = d->ReadDir(count - static_cast<int>(list.size()));
            for (const auto& dir : dirs) {
                auto [info, ierr] = dir->Info();
                // Pretend it doesn't exist, like os::File::Readdir does.
                if (ierr) continue;
                list.push_back(std::move(info));
            }
            if (err) return {std::move(list), err};
            if (count < 0 || static_cast<int>(list.size()) >= count) break;
        }
        return {std::move(list), nullptr};
    }

    os::File* osFile() const noexcept { return dynamic_cast<os::File*>(file_.get()); }

private:
    ptr<fs::File> file_;
};

// The os::File under a file of a Dir or an FS (os::DirFS, an os::Root's),
// if there's one.
os::File* osFile(File& f) {
    if (auto* d = dynamic_cast<dirFile*>(&f)) return d->osFile();
    if (auto* i = dynamic_cast<ioFile*>(&f)) return i->osFile();
    return nullptr;
}

// ioFS: an fs::FS as a FileSystem.
class ioFs final : public FileSystem {
public:
    explicit ioFs(ptr<fs::FS> fsys) : fsys_(std::move(fsys)) {}

    std::pair<ptr<File>, gopp::error> Open(std::string_view name) override {
        if (name == "/") {
            name = ".";
        } else if (name.starts_with('/')) {
            name.remove_prefix(1);
        }
        auto [file, err] = fsys_->Open(name);
        if (err) {
            return {nullptr, mapOpenError(err, name, [this](std::string_view p) { return fs::Stat(*fsys_, p); })};
        }
        return {make_ptr<ioFile>(std::move(file)), nullptr};
    }

private:
    ptr<fs::FS> fsys_;
};

// htmlReplacer
string htmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        // "&#34;" is shorter than "&quot;".
        case '"': out += "&#34;"; break;
        // "&#39;" is shorter than "&apos;" and apos was not in HTML until HTML5.
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return string(out);
}

void dirList(ResponseWriter& w, Request& r, File& f) {
    struct ent {
        string name;
        bool dir;
    };
    std::vector<ent> dirs;
    gopp::error err;
    // Prefer ReadDir to Readdir, which calls Stat on every entry.
    if (auto* d = dynamic_cast<readDirFile*>(&f)) {
        auto [list, e] = d->ReadDir(-1);
        for (const auto& de : list) dirs.push_back({de->Name(), de->IsDir()});
        err = e;
    } else {
        auto [list, e] = f.Readdir(-1);
        for (const auto& fi : list) dirs.push_back({fi->Name(), fi->IsDir()});
        err = e;
    }
    if (err) {
        internal::logf("http: error reading directory: " + std::string(err.Error().view()));
        Error(w, "Error reading directory", StatusInternalServerError);
        return;
    }
    std::sort(dirs.begin(), dirs.end(), [](const ent& a, const ent& b) { return a.name < b.name; });

    w.Header().Set("Content-Type", string::fromStatic("text/html; charset=utf-8"));
    std::string out = "<!doctype html>\n<meta name=\"viewport\" content=\"width=device-width\">\n<pre>\n";
    for (const ent& e : dirs) {
        string name = e.dir ? e.name + "/" : e.name;
        // name may contain '?' or '#', which must be escaped to remain
        // part of the URL path, and not indicate the start of a query
        // string or fragment.
        url::URL u;
        u.Path = name;
        out += "<a href=\"";
        out += u.String().view();
        out += "\">";
        out += htmlEscape(name).view();
        out += "</a>\n";
    }
    out += "</pre>\n";
    io::WriteString(w, out);
}

// serveError: an error of ServeFile, ServeFileFS or ServeContent. Their
// callers may have set headers for a successful response, which an error
// mustn't carry. (Go keeps them with GODEBUG=httpservecontentkeepheaders=1;
// go++ has no GODEBUG.)
void serveError(ResponseWriter& w, std::string_view text, int code) {
    Header& h = w.Header();
    for (std::string_view k : {"Cache-Control", "Content-Encoding", "Etag", "Last-Modified"}) h.Del(k);
    Error(w, text, code);
}

// textproto.TrimString: trims ASCII spaces.
std::string_view trimString(std::string_view s) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

std::pair<std::string_view, std::string_view> internal::scanEtag(std::string_view s) {
    s = trimString(s);
    std::size_t start = 0;
    if (s.starts_with("W/")) start = 2;
    if (s.size() - start < 2 || s[start] != '"') return {{}, {}};
    // An ETag is either W/"text" or "text" (RFC 7232 section 2.3).
    for (std::size_t i = start + 1; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x21 || (c >= 0x23 && c <= 0x7E) || c >= 0x80) continue;  // characters allowed in ETags
        if (c == '"') return {s.substr(0, i + 1), s.substr(i + 1)};
        return {{}, {}};
    }
    return {{}, {}};
}

namespace {

using internal::scanEtag;

// Strong and weak comparisons of valid ETags.
bool etagStrongMatch(std::string_view a, std::string_view b) { return a == b && !a.empty() && a[0] == '"'; }

bool etagWeakMatch(std::string_view a, std::string_view b) {
    if (a.starts_with("W/")) a.remove_prefix(2);
    if (b.starts_with("W/")) b.remove_prefix(2);
    return a == b;
}

// The result of a precondition check (RFC 7232 section 3).
enum class condResult { none, isTrue, isFalse };

condResult checkIfMatch(ResponseWriter& w, Request& r) {
    string imHeader = r.Header.Get("If-Match");
    std::string_view im = imHeader;
    if (im.empty()) return condResult::none;
    string etagHeader = w.Header().Get("Etag");
    for (;;) {
        im = trimString(im);
        if (im.empty()) break;
        if (im[0] == ',') {
            im.remove_prefix(1);
            continue;
        }
        if (im[0] == '*') return condResult::isTrue;
        auto [etag, remain] = scanEtag(im);
        if (etag.empty()) break;
        if (etagStrongMatch(etag, etagHeader)) return condResult::isTrue;
        im = remain;
    }
    return condResult::isFalse;
}

// isZeroTime: whether t is obviously unspecified: zero or the Unix epoch.
bool isZeroTime(const time::Time& t) { return t.IsZero() || t.UnixNano() == 0; }

// The Last-Modified header has whole seconds, so modtime is truncated too.
time::Time truncateSecond(const time::Time& t) {
    int64 ns = t.UnixNano();
    int64 sec = ns / 1000000000 - (ns % 1000000000 < 0 ? 1 : 0);
    return time::Time::Unix(sec, 0);
}

condResult checkIfUnmodifiedSince(Request& r, const time::Time& modtime) {
    string ius = r.Header.Get("If-Unmodified-Since");
    if (ius.empty() || isZeroTime(modtime)) return condResult::none;
    auto [t, err] = ParseTime(ius);
    if (err) return condResult::none;
    if (truncateSecond(modtime).UnixNano() <= t.UnixNano()) return condResult::isTrue;
    return condResult::isFalse;
}

condResult checkIfNoneMatch(ResponseWriter& w, Request& r) {
    string inm = r.Header.Get("If-None-Match");
    if (inm.empty()) return condResult::none;
    string etagHeader = w.Header().Get("Etag");
    std::string_view buf = inm;
    for (;;) {
        buf = trimString(buf);
        if (buf.empty()) break;
        if (buf[0] == ',') {
            buf.remove_prefix(1);
            continue;
        }
        if (buf[0] == '*') return condResult::isFalse;
        auto [etag, remain] = scanEtag(buf);
        if (etag.empty()) break;
        if (etagWeakMatch(etag, etagHeader)) return condResult::isFalse;
        buf = remain;
    }
    return condResult::isTrue;
}

condResult checkIfModifiedSince(Request& r, const time::Time& modtime) {
    if (r.Method != "GET" && r.Method != "HEAD") return condResult::none;
    string ims = r.Header.Get("If-Modified-Since");
    if (ims.empty() || isZeroTime(modtime)) return condResult::none;
    auto [t, err] = ParseTime(ims);
    if (err) return condResult::none;
    if (truncateSecond(modtime).UnixNano() <= t.UnixNano()) return condResult::isFalse;
    return condResult::isTrue;
}

condResult checkIfRange(ResponseWriter& w, Request& r, const time::Time& modtime) {
    if (r.Method != "GET" && r.Method != "HEAD") return condResult::none;
    string ir = r.Header.Get("If-Range");
    if (ir.empty()) return condResult::none;
    auto [etag, remain] = scanEtag(ir);
    if (!etag.empty()) {
        return etagStrongMatch(etag, w.Header().Get("Etag")) ? condResult::isTrue : condResult::isFalse;
    }
    // The If-Range value is typically the ETag value, but it may also be
    // the modtime date. See golang.org/issue/8367.
    if (modtime.IsZero()) return condResult::isFalse;
    auto [t, err] = ParseTime(ir);
    if (err) return condResult::isFalse;
    return t.Unix() == modtime.Unix() ? condResult::isTrue : condResult::isFalse;
}

void setLastModified(ResponseWriter& w, const time::Time& modtime) {
    if (!isZeroTime(modtime)) w.Header().Set("Last-Modified", modtime.FormatHTTP());
}

void writeNotModified(ResponseWriter& w) {
    // RFC 7232 section 4.1: a sender SHOULD NOT generate representation
    // metadata other than the above listed fields unless said metadata
    // exists for the purpose of guiding cache updates (e.g., Last-Modified
    // might be useful if the response does not have an ETag field).
    Header& h = w.Header();
    h.Del("Content-Type");
    h.Del("Content-Length");
    h.Del("Content-Encoding");
    if (!h.Get("Etag").empty()) h.Del("Last-Modified");
    w.WriteHeader(StatusNotModified);
}

// checkPreconditions: evaluates the request's preconditions, carefully
// following RFC 7232 section 6. Returns whether that answered the request
// (304 Not Modified or 412 Precondition Failed), and the Range header to
// honor.
std::pair<bool, string> checkPreconditions(ResponseWriter& w, Request& r, const time::Time& modtime) {
    condResult ch = checkIfMatch(w, r);
    if (ch == condResult::none) ch = checkIfUnmodifiedSince(r, modtime);
    if (ch == condResult::isFalse) {
        w.WriteHeader(StatusPreconditionFailed);
        return {true, {}};
    }
    switch (checkIfNoneMatch(w, r)) {
    case condResult::isFalse:
        if (r.Method == "GET" || r.Method == "HEAD") {
            writeNotModified(w);
        } else {
            w.WriteHeader(StatusPreconditionFailed);
        }
        return {true, {}};
    case condResult::none:
        if (checkIfModifiedSince(r, modtime) == condResult::isFalse) {
            writeNotModified(w);
            return {true, {}};
        }
        break;
    case condResult::isTrue:
        break;
    }
    string rangeHeader = r.Header.Get("Range");
    if (!rangeHeader.empty() && checkIfRange(w, r, modtime) == condResult::isFalse) rangeHeader = {};
    return {false, rangeHeader};
}

// httpRange: a byte range to send.
struct httpRange {
    int64 start = 0, length = 0;

    string contentRange(int64 size) const { return fmt::Sprint("bytes {}-{}/{}", start, start + length - 1, size); }

    // The header of its part of a multipart/byteranges response, as
    // multipart.Writer.CreatePart writes it, with the keys sorted.
    std::string partHeader(std::string_view boundary, bool first, std::string_view contentType, int64 size) const {
        std::string s = first ? "" : "\r\n";
        s += "--";
        s += boundary;
        s += "\r\nContent-Range: ";
        s += contentRange(size).view();
        s += "\r\nContent-Type: ";
        s += contentType;
        s += "\r\n\r\n";
        return s;
    }
};

// The end of a multipart body (multipart.Writer.Close).
std::string multipartClose(std::string_view boundary) { return "\r\n--" + std::string(boundary) + "--\r\n"; }

// multipart.Writer's random boundary: 30 random bytes in hex.
std::string randomBoundary() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 30; i++) {
        unsigned b = rd() & 0xFF;
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}

// The most ranges a Range header may have; more are ignored
// (GODEBUG=httpservecontentmaxranges's default).
constexpr std::size_t maxContentRanges = 200;

// parseRange: the ranges of a Range header, as RFC 7233 has it; nothing
// for an empty header; errNoOverlap if no range starts within the content.
std::pair<std::vector<httpRange>, gopp::error> parseRange(std::string_view s, int64 size) {
    if (s.empty()) return {{}, nullptr};  // header not present
    constexpr std::string_view b = "bytes=";
    if (!s.starts_with(b)) return {{}, errors::New("invalid range")};
    s.remove_prefix(b.size());
    std::vector<httpRange> ranges;
    bool noOverlap = false;
    std::size_t numRanges = static_cast<std::size_t>(std::count(s.begin(), s.end(), ',')) + 1;
    if (numRanges > maxContentRanges) return {{}, nullptr};  // ignore a header with too many ranges
    for (std::size_t pos = 0; pos <= s.size();) {
        auto comma = s.find(',', pos);
        std::string_view ra = trimString(s.substr(pos, comma == s.npos ? s.npos : comma - pos));
        pos = comma == s.npos ? s.size() + 1 : comma + 1;
        if (ra.empty()) continue;
        auto dash = ra.find('-');
        if (dash == ra.npos) return {{}, errors::New("invalid range")};
        std::string_view start = trimString(ra.substr(0, dash)), end = trimString(ra.substr(dash + 1));
        httpRange r;
        if (start.empty()) {
            // If no start is specified, end specifies the range start
            // relative to the end of the file, and we are dealing with
            // <suffix-length>, which has to be a non-negative integer as
            // per RFC 7233 section 2.1 "Byte-Ranges".
            if (end.empty() || end[0] == '-') return {{}, errors::New("invalid range")};
            auto [i, err] = strconv::ParseInt(end, 10, 64);
            if (i < 0 || err) return {{}, errors::New("invalid range")};
            if (i > size) i = size;
            r.start = size - i;
            r.length = size - r.start;
        } else {
            auto [i, err] = strconv::ParseInt(start, 10, 64);
            if (err || i < 0) return {{}, errors::New("invalid range")};
            if (i >= size) {
                // If the range begins after the size of the content, then
                // it does not overlap.
                noOverlap = true;
                continue;
            }
            r.start = i;
            if (end.empty()) {
                // If no end is specified, the range extends to the end of
                // the file.
                r.length = size - r.start;
            } else {
                auto [j, eerr] = strconv::ParseInt(end, 10, 64);
                if (eerr || r.start > j) return {{}, errors::New("invalid range")};
                if (j >= size) j = size - 1;
                r.length = j - r.start + 1;
            }
        }
        ranges.push_back(r);
    }
    // The specified ranges did not overlap with the content.
    if (noOverlap && ranges.empty()) return {{}, errNoOverlap};
    return {std::move(ranges), nullptr};
}

// rangesMIMESize: the length of the ranges as a multipart response.
int64 rangesMimeSize(const std::vector<httpRange>& ranges, std::string_view boundary,
                       std::string_view contentType, int64 contentSize) {
    int64 encSize = 0;
    for (std::size_t i = 0; i < ranges.size(); i++) {
        encSize += static_cast<int64>(ranges[i].partHeader(boundary, i == 0, contentType, contentSize).size());
        encSize += ranges[i].length;
    }
    return encSize + static_cast<int64>(multipartClose(boundary).size());
}

int64 sumRangesSize(const std::vector<httpRange>& ranges) {
    int64 size = 0;
    for (const auto& ra : ranges) size += ra.length;
    return size;
}

// serveContent: an empty name means the file name is unknown (for the
// MIME type, before sniffing); a zero modtime that the modification time
// is. content must be at its start. sizeFn is called at most once, and
// its error, if any, is sent in the response.
void serveContent(ResponseWriter& w, Request& r, std::string_view name, const time::Time& modtime,
                   const std::function<std::pair<int64, gopp::error>()>& sizeFn, io::Reader& content,
                   io::Seeker& seeker) {
    setLastModified(w, modtime);
    auto [done, rangeReq] = checkPreconditions(w, r, modtime);
    if (done) return;

    int code = StatusOK;

    // If Content-Type isn't set, use the file's extension to find it, or
    // else the content. (Go doesn't sniff if Content-Type is set to no
    // values; a go++ Header can't hold a key without a value.)
    string ctype;
    if (!w.Header().Has("Content-Type")) {
        ctype = mime::TypeByExtension(path::Ext(name));
        if (ctype.empty()) {
            // Read a chunk to decide between UTF-8 text and binary.
            std::array<byte, 512> buf;
            auto [n, rerr] = io::ReadFull(content, buf);
            ctype = DetectContentType(std::span<const byte>(buf.data(), static_cast<std::size_t>(n)));
            // Rewind to output the whole file.
            if (seeker.Seek(0, io::SeekStart).second) {
                serveError(w, "seeker can't seek", StatusInternalServerError);
                return;
            }
        }
        w.Header().Set("Content-Type", ctype);
    } else {
        ctype = w.Header().Get("Content-Type");
    }

    auto [size, err] = sizeFn();
    if (err) {
        serveError(w, err.Error(), StatusInternalServerError);
        return;
    }
    if (size < 0) {
        // Should never happen but just to be sure.
        serveError(w, "negative content size computed", StatusInternalServerError);
        return;
    }

    // The Range header.
    int64 sendSize = size;
    auto [ranges, rerr] = parseRange(rangeReq, size);
    if (rerr) {
        if (rerr == errNoOverlap && size == 0) {
            // Some clients add a Range header to all requests to limit the
            // size of the response. If the file is empty, ignore the range
            // header and respond with a 200 rather than a 416.
            ranges.clear();
        } else {
            if (rerr == errNoOverlap) w.Header().Set("Content-Range", fmt::Sprint("bytes */{}", size));
            serveError(w, rerr.Error(), StatusRequestedRangeNotSatisfiable);
            return;
        }
    }

    // The total number of bytes in all the ranges is larger than the size
    // of the file itself, so this is probably an attack, or a dumb client.
    // Ignore the range request.
    if (sumRangesSize(ranges) > size) ranges.clear();

    std::string boundary;
    if (ranges.size() == 1) {
        // RFC 7233 section 4.1: a single part is sent with a Content-Range
        // header, never as a multipart response, which a client that
        // didn't ask for several parts might not support.
        const httpRange& ra = ranges[0];
        if (auto [off, serr] = seeker.Seek(ra.start, io::SeekStart); serr) {
            serveError(w, serr.Error(), StatusRequestedRangeNotSatisfiable);
            return;
        }
        sendSize = ra.length;
        code = StatusPartialContent;
        w.Header().Set("Content-Range", ra.contentRange(size));
    } else if (ranges.size() > 1) {
        boundary = randomBoundary();
        sendSize = rangesMimeSize(ranges, boundary, ctype, size);
        code = StatusPartialContent;
        w.Header().Set("Content-Type", fmt::Sprint("multipart/byteranges; boundary={}", boundary));
    }

    w.Header().Set("Accept-Ranges", string::fromStatic("bytes"));

    // Content-Length is left out when the caller set Content-Encoding:
    // a pattern in the wild wraps the ResponseWriter in one that
    // compresses, which changes the length. A range response always has
    // it, as compressing it was never going to work.
    if (!ranges.empty() || w.Header().Get("Content-Encoding").empty()) {
        w.Header().Set("Content-Length", strconv::Itoa(sendSize));
    }
    w.WriteHeader(code);

    if (r.Method == "HEAD") return;
    if (ranges.size() <= 1) {
        io::CopyN(w, content, sendSize);
        return;
    }
    // The parts, written as they're read (Go copies them through a pipe).
    for (std::size_t i = 0; i < ranges.size(); i++) {
        const httpRange& ra = ranges[i];
        if (io::WriteString(w, ra.partHeader(boundary, i == 0, ctype, size)).second) return;
        if (seeker.Seek(ra.start, io::SeekStart).second) return;
        if (io::CopyN(w, content, ra.length).second) return;
    }
    io::WriteString(w, multipartClose(boundary));
}

// toHTTPError: a non-specific message and status for err, which isn't
// sent itself, so as not to leak information.
std::pair<std::string_view, int> toHttpError(const gopp::error& err) {
    if (errors::Is(err, fs::ErrNotExist)) return {"404 page not found", StatusNotFound};
    if (errors::Is(err, fs::ErrPermission)) return {"403 Forbidden", StatusForbidden};
    if (errors::Is(err, errInvalidUnsafePath)) return {"404 page not found", StatusNotFound};
    return {"500 Internal Server Error", StatusInternalServerError};
}

// localRedirect: a 301 to newPath, which, unlike Redirect, stays
// relative, so that it works under StripPrefix.
void localRedirect(ResponseWriter& w, Request& r, string newPath) {
    // There is no reliable way to redirect correctly when the path has
    // escaped slashes, since StripPrefix might be in use. Just return 404.
    string p = r.URL.EscapedPath();
    if (strings::Contains(p, "%2f") || strings::Contains(p, "%2F")) {
        NotFound(w, r);
        return;
    }
    if (!r.URL.RawQuery.empty()) newPath = newPath + "?" + r.URL.RawQuery;
    w.Header().Set("Location", std::move(newPath));
    w.WriteHeader(StatusMovedPermanently);
}

// serveFile: name is slash-separated.
void serveFile(ResponseWriter& w, Request& r, FileSystem& fsys, string name, bool redirect) {
    constexpr std::string_view indexPage = "/index.html";

    // Redirect .../index.html to .../. Redirect() can't be used: it makes
    // the path absolute, which would break under StripPrefix.
    if (r.URL.Path.ends_with(indexPage)) {
        localRedirect(w, r, string::fromStatic("./"));
        return;
    }

    // The files opened, closed on return (Go's defers).
    struct closer {
        std::vector<ptr<File>> files;
        ~closer() {
            for (auto& f : files) f->Close();
        }
    } opened;

    auto [f, err] = fsys.Open(name);
    if (err) {
        auto [msg, code] = toHttpError(err);
        serveError(w, msg, code);
        return;
    }
    opened.files.push_back(f);

    auto [d, serr] = f->Stat();
    if (serr) {
        auto [msg, code] = toHttpError(serr);
        serveError(w, msg, code);
        return;
    }

    if (redirect) {
        // Redirect to the canonical path: / at the end of a directory URL.
        // r.URL.Path always begins with /.
        const string& url = r.URL.Path;
        if (d->IsDir()) {
            if (!url.ends_with("/")) {
                localRedirect(w, r, path::Base(url) + "/");
                return;
            }
        } else if (url.ends_with("/")) {
            string base = path::Base(url);
            if (base == "/" || base == ".") {
                // The FileSystem maps a path like "/" or "/./" to a file
                // instead of a directory.
                serveError(w, "http: attempting to traverse a non-directory", StatusInternalServerError);
                return;
            }
            localRedirect(w, r, "../" + base);
            return;
        }
    }

    if (d->IsDir()) {
        // Redirect if the directory name doesn't end in a slash.
        const string& url = r.URL.Path;
        if (url.empty() || !url.ends_with("/")) {
            localRedirect(w, r, path::Base(url) + "/");
            return;
        }
        // Use the contents of index.html for the directory, if present.
        string index = strings::TrimSuffix(name, "/") + indexPage;
        if (auto [ff, ferr] = fsys.Open(index); !ferr) {
            opened.files.push_back(ff);
            if (auto [dd, derr] = ff->Stat(); !derr) {
                d = dd;
                f = ff;
            }
        }
    }

    // Still a directory? (We didn't find an index.html file.)
    if (d->IsDir()) {
        if (checkIfModifiedSince(r, d->ModTime()) == condResult::isFalse) {
            writeNotModified(w);
            return;
        }
        setLastModified(w, d->ModTime());
        dirList(w, r, *f);
        return;
    }

    // serveContent checks the modification time. It's given the os::File
    // under a Dir's or an FS's file, if there's one, so that the response
    // copies it to the connection with sendfile(2).
    int64 size = d->Size();
    auto sizeFn = [size] { return std::pair<int64, gopp::error>(size, nullptr); };
    if (os::File* of = osFile(*f)) {
        serveContent(w, r, d->Name(), d->ModTime(), sizeFn, *of, *of);
    } else {
        serveContent(w, r, d->Name(), d->ModTime(), sizeFn, *f, *f);
    }
}

bool isSlash(char c) { return c == '/' || c == '\\'; }

// containsDotDot: whether v has a ".." element, split at either slash.
bool containsDotDot(std::string_view v) {
    if (v.find("..") == v.npos) return false;
    for (std::size_t i = 0; i < v.size();) {
        while (i < v.size() && isSlash(v[i])) i++;
        std::size_t start = i;
        while (i < v.size() && !isSlash(v[i])) i++;
        if (v.substr(start, i - start) == "..") return true;
    }
    return false;
}

class fileHandler final : public Handler {
public:
    explicit fileHandler(ptr<FileSystem> root) : root_(std::move(root)) {}

    void ServeHTTP(ResponseWriter& w, Request& r) override {
        if (!r.URL.Path.starts_with("/")) r.URL.Path = "/" + r.URL.Path;
        serveFile(w, r, *root_, path::Clean(r.URL.Path), true);
    }

private:
    ptr<FileSystem> root_;
};

}  // namespace

std::pair<ptr<File>, gopp::error> Dir::Open(std::string_view name) {
    string p = path::Clean("/" + std::string(name)).slice(1);
    if (p.empty()) p = string::fromStatic(".");
    // filepath.Localize: the name must be valid for the system.
    if (!fs::ValidPath(p) || p.find('\0') != string::npos) return {nullptr, errInvalidUnsafePath};
    std::string_view dir = dir_.empty() ? std::string_view(".") : std::string_view(dir_);
    string fullName = path::Join(dir, p);
    auto [f, err] = os::Open(fullName);
    if (err) return {nullptr, mapOpenError(err, fullName, [](std::string_view p) { return os::Stat(p); })};
    return {make_ptr<dirFile>(std::move(f)), nullptr};
}

ptr<FileSystem> FS(ptr<fs::FS> fsys) { return make_ptr<ioFs>(std::move(fsys)); }

ptr<Handler> FileServer(ptr<FileSystem> root) { return make_ptr<fileHandler>(std::move(root)); }

ptr<Handler> FileServerFS(ptr<fs::FS> root) { return FileServer(FS(std::move(root))); }

void ServeFile(ResponseWriter& w, Request& r, std::string_view name) {
    if (containsDotDot(r.URL.Path)) {
        // Too many programs use r.URL.Path to construct the argument to
        // ServeFile. Reject the request under the assumption that happened
        // here and ".." may not be wanted. (name might not contain "..",
        // if code used path::Join(dir, r.URL.Path), which cleans it.)
        serveError(w, "invalid URL path", StatusBadRequest);
        return;
    }
    auto [dir, file] = path::Split(name);
    Dir d(dir);
    serveFile(w, r, d, file, false);
}

void ServeFileFS(ResponseWriter& w, Request& r, ptr<fs::FS> fsys, std::string_view name) {
    if (containsDotDot(r.URL.Path)) {
        serveError(w, "invalid URL path", StatusBadRequest);
        return;
    }
    ioFs f(std::move(fsys));
    serveFile(w, r, f, string(name), false);
}

namespace detail {

void serveContent(ResponseWriter& w, Request& r, std::string_view name, time::Time modtime, io::Reader& content,
                   io::Seeker& seeker) {
    auto sizeFn = [&seeker]() -> std::pair<int64, gopp::error> {
        auto [size, err] = seeker.Seek(0, io::SeekEnd);
        if (err) return {0, errSeeker};
        if (seeker.Seek(0, io::SeekStart).second) return {0, errSeeker};
        return {size, nullptr};
    };
    http::serveContent(w, r, name, modtime, sizeFn, content, seeker);
}

}  // namespace detail

}  // namespace gopp::http
