#include <gopp/io/fs.hpp>

#include <ctime>
#include <string>

namespace gopp::fs {

const error ErrInvalid = errors::New("invalid argument");
const error ErrPermission = errors::New("permission denied");
const error ErrExist = errors::New("file already exists");
const error ErrNotExist = errors::New("file does not exist");
const error ErrClosed = errors::New("file already closed");

string FileMode::String() const {
    constexpr std::string_view str = "dalTLDpSugct?";
    char buf[32];
    std::size_t w = 0;
    for (std::size_t i = 0; i < str.size(); i++) {
        if (m_ & (1u << (32 - 1 - i))) buf[w++] = str[i];
    }
    if (w == 0) buf[w++] = '-';
    constexpr std::string_view rwx = "rwxrwxrwx";
    for (std::size_t i = 0; i < rwx.size(); i++) {
        buf[w++] = (m_ & (1u << (9 - 1 - i))) ? rwx[i] : '-';
    }
    return string(buf, w);
}

string PathError::Error() const {
    std::string s(Op.view());
    s += ' ';
    s += Path.view();
    s += ": ";
    s += Err.Error().view();
    return string(s);
}

bool ValidPath(std::string_view name) {
    // Go also requires valid UTF-8.
    for (std::size_t i = 0; i < name.size();) {
        auto c = static_cast<unsigned char>(name[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > name.size()) return false;
        for (std::size_t j = 1; j < n; j++) {
            if ((static_cast<unsigned char>(name[i + j]) & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    if (name == ".") return true;
    for (;;) {
        std::size_t i = 0;
        while (i < name.size() && name[i] != '/') i++;
        std::string_view elem = name.substr(0, i);
        if (elem.empty() || elem == "." || elem == "..") return false;
        if (i == name.size()) return true;
        name = name.substr(i + 1);
    }
}

string FormatFileInfo(const FileInfo& info) {
    string name = info.Name();
    std::string b(info.Mode().String().view());
    b += ' ';
    b += std::to_string(info.Size());
    b += ' ';
    b += info.ModTime().Format(time::DateTime).view();
    b += ' ';
    b += name.view();
    if (info.IsDir()) b += '/';
    return string(b);
}

string FormatDirEntry(const DirEntry& dir) {
    string name = dir.Name();
    // The type's String without the permission bits.
    std::string mode(dir.Type().String().view());
    mode.resize(mode.size() - 9);
    std::string b = mode + ' ';
    b += name.view();
    if (dir.IsDir()) b += '/';
    return string(b);
}

}  // namespace gopp::fs
