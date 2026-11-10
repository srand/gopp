#include <gopp/net.hpp>
#include <gopp/os.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gopp::os {

namespace {

// Writes to a file descriptor, blocking the thread like Go's os.File on a
// terminal or pipe.
class fd_writer final : public io::Writer {
public:
    explicit fd_writer(int fd) : fd_(fd) { add_ref(); }  // never freed

    std::pair<isize, error> Write(std::span<const byte> p) override {
        isize n = 0;
        while (!p.empty()) {
            ssize_t w = ::write(fd_, p.data(), p.size());
            if (w < 0) {
                if (errno == EINTR) continue;
                return {n, net::errno_error(errno)};
            }
            n += w;
            p = p.subspan(static_cast<std::size_t>(w));
        }
        return {n, nullptr};
    }

private:
    int fd_;
};

fd_writer& out() {
    static fd_writer* w = new fd_writer(1);
    return *w;
}

fd_writer& err() {
    static fd_writer* w = new fd_writer(2);
    return *w;
}

std::vector<string>& argv_storage() {
    static std::vector<string>* v = new std::vector<string>();
    return *v;
}

}  // namespace

io::Writer& Stdout = out();
io::Writer& Stderr = err();

const std::vector<string>& Args() { return argv_storage(); }

void SetArgs(int argc, char** argv) {
    auto& v = argv_storage();
    v.clear();
    for (int i = 0; i < argc; i++) v.emplace_back(argv[i]);
}

string Getenv(std::string_view key) {
    const char* v = std::getenv(std::string(key).c_str());
    return v ? string(v) : string();
}

void Exit(int code) {
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace gopp::os
