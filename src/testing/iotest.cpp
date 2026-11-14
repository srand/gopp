// Ported from Go 1.27.3's testing/iotest/reader.go (TestReader).
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>
#include <gopp/testing/iotest.hpp>

#include <algorithm>
#include <vector>

namespace gopp::iotest {

namespace {

// Go's smallByteReader: reads 1, 2, 3 bytes, cycling.
struct smallByteReader final : io::Reader {
    io::Reader& r;
    int off = 0;
    int n = 0;
    explicit smallByteReader(io::Reader& r_) : r(r_) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (p.empty()) return {0, nullptr};
        n = n % 3 + 1;
        std::size_t k = std::min<std::size_t>(std::size_t(n), p.size());
        auto [m, err] = r.Read(p.first(k));
        off += int(m);
        return {m, err};
    }
};

string q(std::span<const byte> b) { return strconv::Quote(as_string_view(b)); }

bool equal(std::span<const byte> a, std::span<const byte> b) { return std::ranges::equal(a, b); }

std::pair<slice<byte>, error> readAllSmall(io::Reader& r) {
    smallByteReader s(r);
    return io::ReadAll(s);
}

}  // namespace

error TestReader(io::Reader& r, std::span<const byte> content) {
    const int64 len = int64(content.size());
    if (!content.empty()) {
        auto [n, err] = r.Read({});
        if (n != 0 || err) return fmt::Errorf("Read(0) = {}, {}, want 0, nil", n, err);
    }

    auto [data, err] = readAllSmall(r);
    if (err) return err;
    if (!equal(data, content)) return fmt::Errorf("ReadAll(small amounts) = {}\n\twant {}", q(data), q(content));
    byte ten[10];
    if (auto [n, rerr] = r.Read(ten); n != 0 || rerr != io::EOF_) {
        return fmt::Errorf("Read(10) at EOF = {}, {}, want 0, EOF", n, rerr);
    }

    if (auto* s = dynamic_cast<io::Seeker*>(&r)) {
        // Seek(0, 1) should report the current file position (EOF).
        if (auto [off, serr] = s->Seek(0, 1); off != len || serr) {
            return fmt::Errorf("Seek(0, 1) from EOF = {}, {}, want {}, nil", off, serr, len);
        }

        // Seek backward partway through file, in two steps. If middle == 0,
        // len(content) == 0, can't use the -1 and +1 seeks.
        int64 middle = len - len / 3;
        if (middle > 0) {
            if (auto [off, serr] = s->Seek(-1, 1); off != len - 1 || serr) {
                return fmt::Errorf("Seek(-1, 1) from EOF = {}, {}, want {}, nil", -off, serr, len - 1);
            }
            if (auto [off, serr] = s->Seek(-len / 3, 1); off != middle - 1 || serr) {
                return fmt::Errorf("Seek({}, 1) from {} = {}, {}, want {}, nil", -len / 3, len - 1, off, serr,
                                   middle - 1);
            }
            if (auto [off, serr] = s->Seek(+1, 1); off != middle || serr) {
                return fmt::Errorf("Seek(+1, 1) from {} = {}, {}, want {}, nil", middle - 1, off, serr, middle);
            }
        }

        // Seek(0, 1) should report the current file position (middle).
        if (auto [off, serr] = s->Seek(0, 1); off != middle || serr) {
            return fmt::Errorf("Seek(0, 1) from {} = {}, {}, want {}, nil", middle, off, serr, middle);
        }

        // Reading forward should return the last part of the file.
        auto rest = content.subspan(std::size_t(middle));
        auto [d1, e1] = readAllSmall(r);
        if (e1) return fmt::Errorf("ReadAll from offset {}: {}", middle, e1);
        if (!equal(d1, rest)) return fmt::Errorf("ReadAll from offset {} = {}\n\twant {}", middle, q(d1), q(rest));

        // Seek relative to end of file, but start elsewhere.
        if (auto [off, serr] = s->Seek(middle / 2, 0); off != middle / 2 || serr) {
            return fmt::Errorf("Seek({}, 0) from EOF = {}, {}, want {}, nil", middle / 2, off, serr, middle / 2);
        }
        if (auto [off, serr] = s->Seek(-len / 3, 2); off != middle || serr) {
            return fmt::Errorf("Seek({}, 2) from {} = {}, {}, want {}, nil", -len / 3, middle / 2, off, serr, middle);
        }

        // Reading forward should return the last part of the file (again).
        auto [d2, e2] = readAllSmall(r);
        if (e2) return fmt::Errorf("ReadAll from offset {}: {}", middle, e2);
        if (!equal(d2, rest)) return fmt::Errorf("ReadAll from offset {} = {}\n\twant {}", middle, q(d2), q(rest));

        // Absolute seek & read forward.
        if (auto [off, serr] = s->Seek(middle / 2, 0); off != middle / 2 || serr) {
            return fmt::Errorf("Seek({}, 0) from EOF = {}, {}, want {}, nil", middle / 2, off, serr, middle / 2);
        }
        auto half = content.subspan(std::size_t(middle / 2));
        auto [d3, e3] = io::ReadAll(r);
        if (e3) return fmt::Errorf("ReadAll from offset {}: {}", middle / 2, e3);
        if (!equal(d3, half)) return fmt::Errorf("ReadAll from offset {} = {}\n\twant {}", middle / 2, q(d3), q(half));
    }

    if (auto* ra = dynamic_cast<io::ReaderAt*>(&r)) {
        // len(content) bytes, with room for one more.
        std::vector<byte> buf(content.size() + 1, byte(0xfe));
        std::span<byte> d(buf.data(), content.size());
        auto [n, rerr] = ra->ReadAt(d, 0);
        if (n != isize(d.size()) || (rerr && rerr != io::EOF_)) {
            return fmt::Errorf("ReadAt({}, 0) = {}, {}, want {}, nil or EOF", d.size(), n, rerr, d.size());
        }
        if (!equal(d, content)) return fmt::Errorf("ReadAt({}, 0) = {}\n\twant {}", d.size(), q(d), q(content));

        std::tie(n, rerr) = ra->ReadAt(std::span<byte>(buf.data(), 1), len);
        if (n != 0 || rerr != io::EOF_) return fmt::Errorf("ReadAt(1, {}) = {}, {}, want 0, EOF", len, n, rerr);

        std::ranges::fill(buf, byte(0xfe));
        std::tie(n, rerr) = ra->ReadAt(buf, 0);
        if (n != isize(d.size()) || rerr != io::EOF_) {
            return fmt::Errorf("ReadAt({}, 0) = {}, {}, want {}, EOF", buf.size(), n, rerr, d.size());
        }
        if (!equal(d, content)) return fmt::Errorf("ReadAt({}, 0) = {}\n\twant {}", d.size(), q(d), q(content));

        std::ranges::fill(buf, byte(0xfe));
        for (std::size_t i = 0; i < d.size(); i++) {
            std::tie(n, rerr) = ra->ReadAt(d.subspan(i, 1), int64(i));
            if (n != 1 || (rerr && (i != d.size() - 1 || rerr != io::EOF_))) {
                std::string_view want = i == d.size() - 1 ? "nil or EOF" : "nil";
                return fmt::Errorf("ReadAt(1, {}) = {}, {}, want 1, {}", i, n, rerr, want);
            }
            if (d[i] != content[i]) {
                return fmt::Errorf("ReadAt(1, {}) = {} want {}", i, q(d.subspan(i, 1)), q(content.subspan(i, 1)));
            }
        }
    }
    return nullptr;
}

}  // namespace gopp::iotest
