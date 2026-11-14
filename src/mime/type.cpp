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

    std::vector<string> mime_globs{string::fromStatic("/usr/local/share/mime/globs2"),
                                   string::fromStatic("/usr/share/mime/globs2")};
    std::vector<string> type_files{string::fromStatic("/etc/mime.types"), string::fromStatic("/etc/apache2/mime.types"),
                                   string::fromStatic("/etc/apache/mime.types"),
                                   string::fromStatic("/etc/httpd/conf/mime.types")};
};

tables& tab() {
    static tables* t = new tables;  // never destroyed: lookups may run during exit
    return *t;
}

void initMimeUnix();

void init_mime() {
    tables& t = tab();
    if (t.test_init) {
        t.test_init();
    } else {
        set_mime_types(builtin_types_lower(), builtin_types_lower());
        initMimeUnix();
    }
}

bool containsAny(std::string_view s, std::string_view chars) { return s.find_first_of(chars) != s.npos; }

}  // namespace

const type_map& builtin_types_lower() {
    static const type_map m{
        {string::fromStatic(".ai"), string::fromStatic("application/postscript")},
        {string::fromStatic(".apk"), string::fromStatic("application/vnd.android.package-archive")},
        {string::fromStatic(".apng"), string::fromStatic("image/apng")},
        {string::fromStatic(".avif"), string::fromStatic("image/avif")},
        {string::fromStatic(".bin"), string::fromStatic("application/octet-stream")},
        {string::fromStatic(".bmp"), string::fromStatic("image/bmp")},
        {string::fromStatic(".com"), string::fromStatic("application/octet-stream")},
        {string::fromStatic(".css"), string::fromStatic("text/css; charset=utf-8")},
        {string::fromStatic(".csv"), string::fromStatic("text/csv; charset=utf-8")},
        {string::fromStatic(".doc"), string::fromStatic("application/msword")},
        {string::fromStatic(".docx"),
         string::fromStatic("application/vnd.openxmlformats-officedocument.wordprocessingml.document")},
        {string::fromStatic(".ehtml"), string::fromStatic("text/html; charset=utf-8")},
        {string::fromStatic(".eml"), string::fromStatic("message/rfc822")},
        {string::fromStatic(".eps"), string::fromStatic("application/postscript")},
        {string::fromStatic(".exe"), string::fromStatic("application/octet-stream")},
        {string::fromStatic(".flac"), string::fromStatic("audio/flac")},
        {string::fromStatic(".gif"), string::fromStatic("image/gif")},
        {string::fromStatic(".gz"), string::fromStatic("application/gzip")},
        {string::fromStatic(".htm"), string::fromStatic("text/html; charset=utf-8")},
        {string::fromStatic(".html"), string::fromStatic("text/html; charset=utf-8")},
        {string::fromStatic(".ico"), string::fromStatic("image/vnd.microsoft.icon")},
        {string::fromStatic(".ics"), string::fromStatic("text/calendar; charset=utf-8")},
        {string::fromStatic(".jfif"), string::fromStatic("image/jpeg")},
        {string::fromStatic(".jpeg"), string::fromStatic("image/jpeg")},
        {string::fromStatic(".jpg"), string::fromStatic("image/jpeg")},
        {string::fromStatic(".js"), string::fromStatic("text/javascript; charset=utf-8")},
        {string::fromStatic(".json"), string::fromStatic("application/json")},
        {string::fromStatic(".m4a"), string::fromStatic("audio/mp4")},
        {string::fromStatic(".mjs"), string::fromStatic("text/javascript; charset=utf-8")},
        {string::fromStatic(".mp3"), string::fromStatic("audio/mpeg")},
        {string::fromStatic(".mp4"), string::fromStatic("video/mp4")},
        {string::fromStatic(".oga"), string::fromStatic("audio/ogg")},
        {string::fromStatic(".ogg"), string::fromStatic("audio/ogg")},
        {string::fromStatic(".ogv"), string::fromStatic("video/ogg")},
        {string::fromStatic(".opus"), string::fromStatic("audio/ogg")},
        {string::fromStatic(".pdf"), string::fromStatic("application/pdf")},
        {string::fromStatic(".pjp"), string::fromStatic("image/jpeg")},
        {string::fromStatic(".pjpeg"), string::fromStatic("image/jpeg")},
        {string::fromStatic(".png"), string::fromStatic("image/png")},
        {string::fromStatic(".ppt"), string::fromStatic("application/vnd.ms-powerpoint")},
        {string::fromStatic(".pptx"),
         string::fromStatic("application/vnd.openxmlformats-officedocument.presentationml.presentation")},
        {string::fromStatic(".ps"), string::fromStatic("application/postscript")},
        {string::fromStatic(".rdf"), string::fromStatic("application/rdf+xml")},
        {string::fromStatic(".rtf"), string::fromStatic("application/rtf")},
        {string::fromStatic(".shtml"), string::fromStatic("text/html; charset=utf-8")},
        {string::fromStatic(".svg"), string::fromStatic("image/svg+xml")},
        {string::fromStatic(".text"), string::fromStatic("text/plain; charset=utf-8")},
        {string::fromStatic(".tif"), string::fromStatic("image/tiff")},
        {string::fromStatic(".tiff"), string::fromStatic("image/tiff")},
        {string::fromStatic(".txt"), string::fromStatic("text/plain; charset=utf-8")},
        {string::fromStatic(".vtt"), string::fromStatic("text/vtt; charset=utf-8")},
        {string::fromStatic(".wasm"), string::fromStatic("application/wasm")},
        {string::fromStatic(".wav"), string::fromStatic("audio/wav")},
        {string::fromStatic(".weba"), string::fromStatic("audio/webm")},
        {string::fromStatic(".webm"), string::fromStatic("video/webm")},
        {string::fromStatic(".webp"), string::fromStatic("image/webp")},
        {string::fromStatic(".xbl"), string::fromStatic("text/xml; charset=utf-8")},
        {string::fromStatic(".xbm"), string::fromStatic("image/x-xbitmap")},
        {string::fromStatic(".xht"), string::fromStatic("application/xhtml+xml")},
        {string::fromStatic(".xhtml"), string::fromStatic("application/xhtml+xml")},
        {string::fromStatic(".xls"), string::fromStatic("application/vnd.ms-excel")},
        {string::fromStatic(".xlsx"),
         string::fromStatic("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")},
        {string::fromStatic(".xml"), string::fromStatic("text/xml; charset=utf-8")},
        {string::fromStatic(".xsl"), string::fromStatic("text/xml; charset=utf-8")},
        {string::fromStatic(".zip"), string::fromStatic("application/zip")},
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
    if (mime_type.starts_with("text/") && param[string::fromStatic("charset")].empty()) {
        param[string::fromStatic("charset")] = string::fromStatic("utf-8");
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
error loadMimeGlobsFile(std::string_view filename) {
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
        if (containsAny(extension, "?*[")) continue;
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

void initMimeUnix() {
    tables& t = tab();
    for (const string& filename : t.mime_globs) {
        // Stop checking more files if the MIME-type database is found.
        if (!loadMimeGlobsFile(filename)) return;
    }
    // Fallback if no system-generated MIME-type database exists.
    for (const string& filename : t.type_files) load_mime_file(filename);
}

}  // namespace

void ensureInit() {
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

std::pair<std::vector<string>, std::vector<string>> setSystemFiles(std::vector<string> globs,
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
    internal::ensureInit();
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
    internal::ensureInit();
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
    internal::ensureInit();
    return internal::set_extension_type(ext, typ);
}

}  // namespace gopp::mime
