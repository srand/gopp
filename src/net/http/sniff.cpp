// Content-type sniffing, after Go's net/http/sniff.Go (the common
// signatures of the WHATWG MIME Sniffing Standard).
#include <gopp/net/http.hpp>

#include <cstring>

namespace gopp::http {

namespace {

constexpr std::size_t sniffLen = 512;

bool isWs(byte b) noexcept { return b == '\t' || b == '\n' || b == '\x0c' || b == '\r' || b == ' '; }
bool isTt(byte b) noexcept { return b == ' ' || b == '>'; }  // tag-terminating byte

bool HasPrefix(std::span<const byte> d, std::string_view p) noexcept {
    return d.size() >= p.size() && std::memcmp(d.data(), p.data(), p.size()) == 0;
}

// An HTML tag at the start of d, case-insensitively, then a tag terminator.
bool htmlSig(std::span<const byte> d, std::string_view tag) noexcept {
    if (d.size() < tag.size() + 1) return false;
    for (std::size_t i = 0; i < tag.size(); i++) {
        byte c = d[i];
        byte t = static_cast<byte>(tag[i]);
        if (t >= 'A' && t <= 'Z') c &= 0xDF;  // uppercase
        if (c != t) return false;
    }
    return isTt(d[tag.size()]);
}

}  // namespace

string DetectContentType(std::span<const byte> data) {
    if (data.size() > sniffLen) data = data.first(sniffLen);

    std::size_t firstNonWs = 0;
    while (firstNonWs < data.size() && isWs(data[firstNonWs])) firstNonWs++;
    auto text = data.subspan(firstNonWs);

    static constexpr std::string_view htmlTags[] = {
        "<!DOCTYPE HTML", "<HTML", "<HEAD", "<SCRIPT", "<IFRAME", "<H1", "<DIV", "<FONT", "<TABLE",
        "<A", "<STYLE", "<TITLE", "<B", "<BODY", "<BR", "<P", "<!--",
    };
    for (auto tag : htmlTags) {
        if (htmlSig(text, tag)) return string::fromStatic("text/html; charset=utf-8");
    }
    if (HasPrefix(text, "<?xml")) return string::fromStatic("text/xml; charset=utf-8");
    if (HasPrefix(data, "%PDF-")) return string::fromStatic("application/pdf");
    if (HasPrefix(data, "%!PS-Adobe-")) return string::fromStatic("application/postscript");
    if (HasPrefix(data, "\xFE\xFF") || HasPrefix(data, "\xFF\xFE")) return string::fromStatic("text/plain; charset=utf-16");
    if (HasPrefix(data, "\xEF\xBB\xBF")) return string::fromStatic("text/plain; charset=utf-8");
    if (HasPrefix(data, "GIF87a") || HasPrefix(data, "GIF89a")) return string::fromStatic("image/gif");
    if (HasPrefix(data, "\x89PNG\x0D\x0A\x1A\x0A")) return string::fromStatic("image/png");
    if (HasPrefix(data, "\xFF\xD8\xFF")) return string::fromStatic("image/jpeg");
    if (data.size() >= 14 && HasPrefix(data, "RIFF") && std::memcmp(data.data() + 8, "WEBPVP", 6) == 0)
        return string::fromStatic("image/webp");
    if (HasPrefix(data, "BM")) return string::fromStatic("image/bmp");
    if (HasPrefix(data, std::string_view("\x00\x00\x01\x00", 4))) return string::fromStatic("image/x-icon");
    if (HasPrefix(data, "OggS\x00")) return string::fromStatic("application/ogg");
    if (HasPrefix(data, "\x1A\x45\xDF\xA3")) return string::fromStatic("video/webm");
    if (HasPrefix(data, "PK\x03\x04")) return string::fromStatic("application/zip");
    if (HasPrefix(data, "\x1F\x8B\x08")) return string::fromStatic("application/x-gzip");
    if (HasPrefix(data, "Rar!\x1A\x07")) return string::fromStatic("application/x-rar-compressed");
    if (HasPrefix(data, std::string_view("\x00\x61\x73\x6D", 4))) return string::fromStatic("application/wasm");

    // Text unless it has binary data bytes.
    for (byte b : data) {
        if (b <= 0x08 || b == 0x0B || (b >= 0x0E && b <= 0x1A) || (b >= 0x1C && b <= 0x1F)) {
            return string::fromStatic("application/octet-stream");
        }
    }
    return string::fromStatic("text/plain; charset=utf-8");
}

}  // namespace gopp::http
