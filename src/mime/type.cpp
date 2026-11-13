// After Go's mime/type.go and mime/type_unix.go.
#include "internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace gopp::mime {

namespace internal {

namespace {

// The tables. The maps are guarded by mu, which is never held across I/O,
// so that a fiber can't park holding it.
struct tables {
    std::shared_mutex mu;
    std::unordered_map<string, string> mime_types;        // ".Z" => "application/x-compress"
    std::unordered_map<string, string> mime_types_lower;  // ".z" => "application/x-compress"
    // MIME type to lowercase extensions: "image/jpeg" => [".jfif", ".jpg", ...]
    std::unordered_map<string, std::vector<string>> extensions;

    // Initialization (Go's once): init_mu is a sync::Mutex, as the system
    // files are read while it's held.
    sync::Mutex init_mu;
    std::atomic<bool> inited{false};
    std::function<void()> test_init;

    std::vector<string> mime_globs{string::from_static("/usr/local/share/mime/globs2"),
                                   string::from_static("/usr/share/mime/globs2")};
    std::vector<string> type_files{string::from_static("/etc/mime.types"), string::from_static("/etc/apache2/mime.types"),
                                   string::from_static("/etc/apache/mime.types"),
                                   string::from_static("/etc/httpd/conf/mime.types")};
};

tables& tab() {
    static tables* t = new tables;  // never destroyed: lookups may run during exit
    return *t;
}

void init_mime_unix();

void init_mime() {
    tables& t = tab();
    if (t.test_init) {
        t.test_init();
    } else {
        set_mime_types(builtin_types_lower(), builtin_types_lower());
        init_mime_unix();
    }
}

bool contains_any(std::string_view s, std::string_view chars) { return s.find_first_of(chars) != s.npos; }

}  // namespace

const type_map& builtin_types_lower() {
    static const type_map m{
        {string::from_static(".ai"), string::from_static("application/postscript")},
        {string::from_static(".apk"), string::from_static("application/vnd.android.package-archive")},
        {string::from_static(".apng"), string::from_static("image/apng")},
        {string::from_static(".avif"), string::from_static("image/avif")},
        {string::from_static(".bin"), string::from_static("application/octet-stream")},
        {string::from_static(".bmp"), string::from_static("image/bmp")},
        {string::from_static(".com"), string::from_static("application/octet-stream")},
        {string::from_static(".css"), string::from_static("text/css; charset=utf-8")},
        {string::from_static(".csv"), string::from_static("text/csv; charset=utf-8")},
        {string::from_static(".doc"), string::from_static("application/msword")},
        {string::from_static(".docx"),
         string::from_static("application/vnd.openxmlformats-officedocument.wordprocessingml.document")},
        {string::from_static(".ehtml"), string::from_static("text/html; charset=utf-8")},
        {string::from_static(".eml"), string::from_static("message/rfc822")},
        {string::from_static(".eps"), string::from_static("application/postscript")},
        {string::from_static(".exe"), string::from_static("application/octet-stream")},
        {string::from_static(".flac"), string::from_static("audio/flac")},
        {string::from_static(".gif"), string::from_static("image/gif")},
        {string::from_static(".gz"), string::from_static("application/gzip")},
        {string::from_static(".htm"), string::from_static("text/html; charset=utf-8")},
        {string::from_static(".html"), string::from_static("text/html; charset=utf-8")},
        {string::from_static(".ico"), string::from_static("image/vnd.microsoft.icon")},
        {string::from_static(".ics"), string::from_static("text/calendar; charset=utf-8")},
        {string::from_static(".jfif"), string::from_static("image/jpeg")},
        {string::from_static(".jpeg"), string::from_static("image/jpeg")},
        {string::from_static(".jpg"), string::from_static("image/jpeg")},
        {string::from_static(".js"), string::from_static("text/javascript; charset=utf-8")},
        {string::from_static(".json"), string::from_static("application/json")},
        {string::from_static(".m4a"), string::from_static("audio/mp4")},
        {string::from_static(".mjs"), string::from_static("text/javascript; charset=utf-8")},
        {string::from_static(".mp3"), string::from_static("audio/mpeg")},
        {string::from_static(".mp4"), string::from_static("video/mp4")},
        {string::from_static(".oga"), string::from_static("audio/ogg")},
        {string::from_static(".ogg"), string::from_static("audio/ogg")},
        {string::from_static(".ogv"), string::from_static("video/ogg")},
        {string::from_static(".opus"), string::from_static("audio/ogg")},
        {string::from_static(".pdf"), string::from_static("application/pdf")},
        {string::from_static(".pjp"), string::from_static("image/jpeg")},
        {string::from_static(".pjpeg"), string::from_static("image/jpeg")},
        {string::from_static(".png"), string::from_static("image/png")},
        {string::from_static(".ppt"), string::from_static("application/vnd.ms-powerpoint")},
        {string::from_static(".pptx"),
         string::from_static("application/vnd.openxmlformats-officedocument.presentationml.presentation")},
        {string::from_static(".ps"), string::from_static("application/postscript")},
        {string::from_static(".rdf"), string::from_static("application/rdf+xml")},
        {string::from_static(".rtf"), string::from_static("application/rtf")},
        {string::from_static(".shtml"), string::from_static("text/html; charset=utf-8")},
        {string::from_static(".svg"), string::from_static("image/svg+xml")},
        {string::from_static(".text"), string::from_static("text/plain; charset=utf-8")},
        {string::from_static(".tif"), string::from_static("image/tiff")},
        {string::from_static(".tiff"), string::from_static("image/tiff")},
        {string::from_static(".txt"), string::from_static("text/plain; charset=utf-8")},
        {string::from_static(".vtt"), string::from_static("text/vtt; charset=utf-8")},
        {string::from_static(".wasm"), string::from_static("application/wasm")},
        {string::from_static(".wav"), string::from_static("audio/wav")},
        {string::from_static(".weba"), string::from_static("audio/webm")},
        {string::from_static(".webm"), string::from_static("video/webm")},
        {string::from_static(".webp"), string::from_static("image/webp")},
        {string::from_static(".xbl"), string::from_static("text/xml; charset=utf-8")},
        {string::from_static(".xbm"), string::from_static("image/x-xbitmap")},
        {string::from_static(".xht"), string::from_static("application/xhtml+xml")},
        {string::from_static(".xhtml"), string::from_static("application/xhtml+xml")},
        {string::from_static(".xls"), string::from_static("application/vnd.ms-excel")},
        {string::from_static(".xlsx"),
         string::from_static("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")},
        {string::from_static(".xml"), string::from_static("text/xml; charset=utf-8")},
        {string::from_static(".xsl"), string::from_static("text/xml; charset=utf-8")},
        {string::from_static(".zip"), string::from_static("application/zip")},
    };
    return m;
}

void set_mime_types(const type_map& lower_ext, const type_map& mix_ext) {
    tables& t = tab();
    std::unique_lock lock(t.mu);
    t.mime_types.clear();
    t.mime_types_lower.clear();
    t.extensions.clear();
    for (const auto& [k, v] : lower_ext) t.mime_types_lower[k] = v;
    for (const auto& [k, v] : mix_ext) t.mime_types[k] = v;
    for (const auto& [k, v] : lower_ext) {
        auto [just_type, params, err] = ParseMediaType(v);
        if (err) panic(err.Error());
        t.extensions[just_type].push_back(k);
    }
}

error set_extension_type(std::string_view extension, std::string_view mime_type) {
    auto [just_type, param, err] = ParseMediaType(mime_type);
    if (err) return err;
    string type(mime_type);
    if (mime_type.starts_with("text/") && param[string::from_static("charset")].empty()) {
        param[string::from_static("charset")] = string::from_static("utf-8");
        type = FormatMediaType(just_type, param);
    }
    string ext(extension);
    string ext_lower = strings::ToLower(extension);

    tables& t = tab();
    std::unique_lock lock(t.mu);
    t.mime_types[ext] = type;
    t.mime_types_lower[ext_lower] = type;
    auto& exts = t.extensions[just_type];
    if (std::find(exts.begin(), exts.end(), ext_lower) == exts.end()) exts.push_back(ext_lower);
    return nullptr;
}

// See https://specifications.freedesktop.org/shared-mime-info-spec/ for
// the FreeDesktop Shared MIME-info Database.
error load_mime_globs_file(std::string_view filename) {
    auto [data, err] = os::ReadFile(filename);
    if (err) return err;
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    while (!text.empty()) {
        auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == text.npos ? std::string_view() : text.substr(nl + 1);

        // Each line should be of format: weight:mimetype:glob[:morefields...]
        std::vector<std::string_view> fields;
        for (std::size_t start = 0;;) {
            auto c = line.find(':', start);
            fields.push_back(line.substr(start, c == line.npos ? line.npos : c - start));
            if (c == line.npos) break;
            start = c + 1;
        }
        if (fields.size() < 3 || fields[0].empty() || fields[2].size() < 3) continue;
        if (fields[0][0] == '#' || fields[2][0] != '*' || fields[2][1] != '.') continue;

        std::string_view extension = fields[2].substr(1);
        // A glob rather than a bare extension: ignored, as Go does.
        if (contains_any(extension, "?*[")) continue;
        {
            tables& t = tab();
            std::shared_lock lock(t.mu);
            // The file is in weight order: the first entry counts.
            if (t.mime_types.contains(string(extension))) continue;
        }
        set_extension_type(extension, fields[1]);
    }
    return nullptr;
}

void load_mime_file(std::string_view filename) {
    auto [data, err] = os::ReadFile(filename);
    if (err) return;
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    while (!text.empty()) {
        auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == text.npos ? std::string_view() : text.substr(nl + 1);

        // strings.Fields
        std::vector<std::string_view> fields;
        for (std::size_t i = 0; i < line.size();) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
            std::size_t start = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
            if (i > start) fields.push_back(line.substr(start, i - start));
        }
        if (fields.size() <= 1 || fields[0][0] == '#') continue;
        std::string_view mime_type = fields[0];
        for (std::size_t i = 1; i < fields.size(); i++) {
            if (fields[i][0] == '#') break;
            set_extension_type(fmt::Sprint(".{}", fields[i]).view(), mime_type);
        }
    }
}

namespace {

void init_mime_unix() {
    tables& t = tab();
    for (const string& filename : t.mime_globs) {
        // Stop checking more files if the MIME-type database is found.
        if (!load_mime_globs_file(filename)) return;
    }
    // Fallback if no system-generated MIME-type database exists.
    for (const string& filename : t.type_files) load_mime_file(filename);
}

}  // namespace

void ensure_init() {
    tables& t = tab();
    if (t.inited.load(std::memory_order_acquire)) return;
    std::lock_guard lock(t.init_mu);
    if (t.inited.load(std::memory_order_relaxed)) return;
    init_mime();
    t.inited.store(true, std::memory_order_release);
}

void set_mime_init(std::function<void()> fn) {
    tables& t = tab();
    std::lock_guard lock(t.init_mu);
    t.test_init = std::move(fn);
    t.inited.store(false, std::memory_order_release);
}

std::pair<std::vector<string>, std::vector<string>> set_system_files(std::vector<string> globs,
                                                                     std::vector<string> type_files) {
    tables& t = tab();
    std::lock_guard lock(t.init_mu);
    auto old = std::make_pair(std::move(t.mime_globs), std::move(t.type_files));
    t.mime_globs = std::move(globs);
    t.type_files = std::move(type_files);
    return old;
}

}  // namespace internal

string TypeByExtension(std::string_view ext) {
    internal::ensure_init();
    auto& t = internal::tab();
    std::shared_lock lock(t.mu);
    // Case-sensitive lookup.
    if (auto it = t.mime_types.find(string(ext)); it != t.mime_types.end()) return it->second;
    // Case-insensitive lookup.
    if (auto it = t.mime_types_lower.find(strings::ToLower(ext)); it != t.mime_types_lower.end()) return it->second;
    return {};
}

std::pair<std::vector<string>, error> ExtensionsByType(std::string_view typ) {
    auto [just_type, params, err] = ParseMediaType(typ);
    if (err) return {{}, err};
    internal::ensure_init();
    auto& t = internal::tab();
    std::vector<string> ret;
    {
        std::shared_lock lock(t.mu);
        auto it = t.extensions.find(just_type);
        if (it == t.extensions.end()) return {{}, nullptr};
        ret = it->second;
    }
    std::sort(ret.begin(), ret.end());
    return {std::move(ret), nullptr};
}

error AddExtensionType(std::string_view ext, std::string_view typ) {
    if (!ext.starts_with('.')) return fmt::Errorf("mime: extension {} missing leading dot", strconv::Quote(ext));
    internal::ensure_init();
    return internal::set_extension_type(ext, typ);
}

}  // namespace gopp::mime
