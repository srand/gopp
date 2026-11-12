#include <gopp/io/fs.hpp>
#include <gopp/syscall.hpp>

#include <cerrno>
#include <cstring>
#include <string>

namespace gopp::syscall {

string Errno::Error() const {
    // Go's errno texts are strerror's, starting in lowercase.
    char buf[128];
    std::string s(strerror_r(Code, buf, sizeof(buf)));
    if (s.size() > 1 && s[0] >= 'A' && s[0] <= 'Z' && !(s[1] >= 'A' && s[1] <= 'Z')) s[0] = static_cast<char>(s[0] + 32);
    return string(s);
}

bool Errno::Timeout() const noexcept { return Code == EAGAIN || Code == ETIMEDOUT; }

bool Errno::Is(const error_interface* target) const noexcept {
    if (target == fs::ErrPermission.get()) return Code == EACCES || Code == EPERM;
    if (target == fs::ErrExist.get()) return Code == EEXIST || Code == ENOTEMPTY;
    if (target == fs::ErrNotExist.get()) return Code == ENOENT;
    return false;
}

gopp::error errno_error(int code) { return make_ptr<Errno>(code); }

}  // namespace gopp::syscall
