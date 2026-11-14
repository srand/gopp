// crypto/rand on getrandom(2).
#include <gopp/crypto/rand.hpp>

#include "runtime/runtime.hpp"

#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace gopp::rand {

namespace {

struct reader final : io::Reader {
    std::pair<isize, error> Read(std::span<byte> p) override { return rand::Read(p); }
};

reader& theReader() {
    // Never destroyed, and counted once so that ptrs to it never free it.
    static reader* r = [] {
        auto* x = new reader;
        x->addRef();
        return x;
    }();
    return *r;
}

}  // namespace

io::Reader& Reader = theReader();

std::pair<isize, error> Read(std::span<byte> b) {
    std::size_t n = 0;
    while (n < b.size()) {
        ssize_t r = ::getrandom(b.data() + n, b.size() - n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            std::string msg = "crypto/rand: failed to read random data (see https://go.dev/issue/66821): ";
            msg += std::strerror(errno);
            runtime::fatal(msg.c_str());
        }
        n += static_cast<std::size_t>(r);
    }
    return {static_cast<isize>(b.size()), nullptr};
}

string Text() {
    static constexpr std::string_view base32alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    // ⌈log₃₂ 2¹²⁸⌉ = 26 chars
    byte src[26];
    Read(src);
    return string::build(sizeof(src), [&](char* p) {
        for (std::size_t i = 0; i < sizeof(src); i++) p[i] = base32alphabet[src[i] % 32];
    });
}

}  // namespace gopp::rand
