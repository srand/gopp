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
    std::unordered_map<string, string> mimeTypes;        // ".Z" => "application/x-compress"
    std::unordered_map<string, string> mimeTypesLower;  // ".z" => "application/x-compress"
    // MIME type to lowercase extensions: "image/jpeg" => [".jfif", ".jpg", ...]
    std::unordered_map<string, std::vector<string>> extensions;

    // Initialization (Go's once): initMu is a sync::Mutex, as the system
    // files are read while it's held.
    sync::Mutex initMu;
    std::atomic<bool> inited{false};
    std::function<void()> testInit;

    std::vector<string> mimeGlobs{string::fromStatic("/usr/local/share/mime/globs2"),
                                   string::fromStatic("/usr/share/mime/globs2")};
    std::vector<string> typeFiles{string::fromStatic("/etc/mime.types"), string::fromStatic("/etc/apache2/mime.types"),
                                   string::fromStatic("/etc/apache/mime.types"),
                                   string::fromStatic("/etc/httpd/conf/mime.types")};
};

tables& tab() {
    static tables* t = new tables;  // never destroyed: lookups may run during exit
    return *t;
}

void initMimeUnix();

void initMime() {
    tables& t = tab();
    if (t.testInit) {
        t.testInit();
    } else {
        setMimeTypes(builtinTypesLower(), builtinTypesLower());
        initMimeUnix();
    }
}

bool containsAny(std::string_view s, std::string_view chars) { return s.find_first_of(chars) != s.npos; }

}  // namespace

const typeMap& builtinTypesLower() {
    static const typeMap m{
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

void setMimeTypes(const typeMap& lowerExt, const typeMap& mixExt) {
    tables& t = tab();
    std::unique_lock lock(t.mu);
    t.mimeTypes.clear();
    t.mimeTypesLower.clear();
    t.extensions.clear();
    for (const auto& [k, v] : lowerExt) t.mimeTypesLower[k] = v;
    for (const auto& [k, v] : mixExt) t.mimeTypes[k] = v;
    for (const auto& [k, v] : lowerExt) {
        auto [justType, params, err] = ParseMediaType(v);
        if (err) panic(err.Error());
        t.extensions[justType].push_back(k);
    }
}

error setExtensionType(std::string_view extension, std::string_view mimeType) {
    auto [justType, param, err] = ParseMediaType(mimeType);
    if (err) return err;
    string type(mimeType);
    if (mimeType.starts_with("text/") && param[string::fromStatic("charset")].empty()) {
        param[string::fromStatic("charset")] = string::fromStatic("utf-8");
        type = FormatMediaType(justType, param);
    }
    string ext(extension);
    string extLower = strings::ToLower(extension);

    tables& t = tab();
    std::unique_lock lock(t.mu);
    t.mimeTypes[ext] = type;
    t.mimeTypesLower[extLower] = type;
    auto& exts = t.extensions[justType];
    if (std::find(exts.begin(), exts.end(), extLower) == exts.end()) exts.push_back(extLower);
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
            if (t.mimeTypes.contains(string(extension))) continue;
        }
        setExtensionType(extension, fields[1]);
    }
    return nullptr;
}

void loadMimeFile(std::string_view filename) {
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
        std::string_view mimeType = fields[0];
        for (std::size_t i = 1; i < fields.size(); i++) {
            if (fields[i][0] == '#') break;
            setExtensionType(fmt::Sprint(".{}", fields[i]).view(), mimeType);
        }
    }
}

namespace {

void initMimeUnix() {
    tables& t = tab();
    for (const string& filename : t.mimeGlobs) {
        // Stop checking more files if the MIME-type database is found.
        if (!loadMimeGlobsFile(filename)) return;
    }
    // Fallback if no system-generated MIME-type database exists.
    for (const string& filename : t.typeFiles) loadMimeFile(filename);
}

}  // namespace

void ensureInit() {
    tables& t = tab();
    if (t.inited.load(std::memory_order_acquire)) return;
    std::lock_guard lock(t.initMu);
    if (t.inited.load(std::memory_order_relaxed)) return;
    initMime();
    t.inited.store(true, std::memory_order_release);
}

void setMimeInit(std::function<void()> fn) {
    tables& t = tab();
    std::lock_guard lock(t.initMu);
    t.testInit = std::move(fn);
    t.inited.store(false, std::memory_order_release);
}

std::pair<std::vector<string>, std::vector<string>> setSystemFiles(std::vector<string> globs,
                                                                     std::vector<string> typeFiles) {
    tables& t = tab();
    std::lock_guard lock(t.initMu);
    auto old = std::make_pair(std::move(t.mimeGlobs), std::move(t.typeFiles));
    t.mimeGlobs = std::move(globs);
    t.typeFiles = std::move(typeFiles);
    return old;
}

}  // namespace internal

string TypeByExtension(std::string_view ext) {
    internal::ensureInit();
    auto& t = internal::tab();
    std::shared_lock lock(t.mu);
    // Case-sensitive lookup.
    if (auto it = t.mimeTypes.find(string(ext)); it != t.mimeTypes.end()) return it->second;
    // Case-insensitive lookup.
    if (auto it = t.mimeTypesLower.find(strings::ToLower(ext)); it != t.mimeTypesLower.end()) return it->second;
    return {};
}

std::pair<std::vector<string>, error> ExtensionsByType(std::string_view typ) {
    auto [justType, params, err] = ParseMediaType(typ);
    if (err) return {{}, err};
    internal::ensureInit();
    auto& t = internal::tab();
    std::vector<string> ret;
    {
        std::shared_lock lock(t.mu);
        auto it = t.extensions.find(justType);
        if (it == t.extensions.end()) return {{}, nullptr};
        ret = it->second;
    }
    std::sort(ret.begin(), ret.end());
    return {std::move(ret), nullptr};
}

error AddExtensionType(std::string_view ext, std::string_view typ) {
    if (!ext.starts_with('.')) return fmt::Errorf("mime: extension {} missing leading dot", strconv::Quote(ext));
    internal::ensureInit();
    return internal::setExtensionType(ext, typ);
}

}  // namespace gopp::mime
