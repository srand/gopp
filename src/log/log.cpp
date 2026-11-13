// The log package, after Go 1.27.3's log/log.go.
#include <gopp/log.hpp>
#include <gopp/os.hpp>
#include <gopp/time.hpp>

#include <mutex>

namespace gopp::log {

namespace {

// Appends i in decimal, zero-padded to wid digits (Go's itoa); a negative
// wid means no padding.
void itoa(std::string& buf, int i, int wid) {
    char b[20];
    int bp = static_cast<int>(sizeof(b)) - 1;
    while (i >= 10 || wid > 1) {
        wid--;
        int q = i / 10;
        b[bp--] = static_cast<char>('0' + i - q * 10);
        i = q;
    }
    b[bp] = static_cast<char>('0' + i);
    buf.append(b + bp, sizeof(b) - static_cast<std::size_t>(bp));
}

void format_header(std::string& buf, time::Time t, std::string_view prefix, int flag, std::string_view file, int line) {
    if ((flag & Lmsgprefix) == 0) buf += prefix;
    if (flag & (Ldate | Ltime | Lmicroseconds)) {
        if (flag & LUTC) t = t.UTC();
        if (flag & Ldate) {
            auto [year, month, day] = t.Date();
            itoa(buf, year, 4);
            buf.push_back('/');
            itoa(buf, static_cast<int>(month), 2);
            buf.push_back('/');
            itoa(buf, day, 2);
            buf.push_back(' ');
        }
        if (flag & (Ltime | Lmicroseconds)) {
            auto [hour, min, sec] = t.Clock();
            itoa(buf, hour, 2);
            buf.push_back(':');
            itoa(buf, min, 2);
            buf.push_back(':');
            itoa(buf, sec, 2);
            if (flag & Lmicroseconds) {
                buf.push_back('.');
                itoa(buf, t.Nanosecond() / 1000, 6);
            }
            buf.push_back(' ');
        }
    }
    if (flag & (Lshortfile | Llongfile)) {
        if (flag & Lshortfile) {
            if (auto i = file.rfind('/'); i != file.npos && i > 0) file = file.substr(i + 1);
        }
        buf += file;
        buf.push_back(':');
        itoa(buf, line, -1);
        buf += ": ";
    }
    if (flag & Lmsgprefix) buf += prefix;
}

}  // namespace

Logger::Logger(ptr<io::Writer> out, string prefix, int flag) {
    SetOutput(std::move(out));
    SetPrefix(std::move(prefix));
    SetFlags(flag);
}

void Logger::SetOutput(ptr<io::Writer> w) {
    std::lock_guard lock(out_mu_);
    is_discard_.store(w.get() == &io::Discard(), std::memory_order_relaxed);
    out_ = std::move(w);
}

ptr<io::Writer> Logger::Writer() const {
    std::lock_guard lock(out_mu_);
    return out_;
}

string Logger::Prefix() const {
    auto p = prefix_.load();
    return p ? *p : string();
}

void Logger::SetPrefix(string prefix) { prefix_.store(std::make_shared<const string>(std::move(prefix))); }

error Logger::output(const std::source_location& loc, std::string_view s) {
    if (discard()) return nullptr;
    time::Time now = time::Now();  // get this early
    string prefix = Prefix();
    int flag = Flags();
    std::string_view file;
    int line = 0;
    if (flag & (Lshortfile | Llongfile)) {
        file = loc.file_name();
        line = static_cast<int>(loc.line());
        if (file.empty()) {
            file = "???";
            line = 0;
        }
    }

    std::string buf;
    buf.reserve(prefix.size() + s.size() + 64);
    format_header(buf, now, prefix, flag, file, line);
    buf += s;
    if (buf.empty() || buf.back() != '\n') buf.push_back('\n');

    std::lock_guard lock(out_mu_);
    if (!out_) return nullptr;
    auto p = std::span<const byte>(reinterpret_cast<const byte*>(buf.data()), buf.size());
    if (auto* lw = dynamic_cast<detail::located_writer*>(out_.get())) return lw->WriteLocated(p, loc).second;
    return out_->Write(p).second;
}

void Logger::exit() { os::Exit(1); }

ptr<Logger> New(ptr<io::Writer> out, string prefix, int flag) {
    return make_ptr<Logger>(std::move(out), std::move(prefix), flag);
}

ptr<Logger> Default() {
    // Made on first use: os::Stderr is a global of another file.
    static const ptr<Logger> std_logger = New(ptr<io::Writer>(&os::Stderr), "", LstdFlags);
    return std_logger;
}

void SetOutput(ptr<io::Writer> w) { Default()->SetOutput(std::move(w)); }
ptr<io::Writer> Writer() { return Default()->Writer(); }
int Flags() { return Default()->Flags(); }
void SetFlags(int flag) { Default()->SetFlags(flag); }
string Prefix() { return Default()->Prefix(); }
void SetPrefix(string prefix) { Default()->SetPrefix(std::move(prefix)); }

}  // namespace gopp::log
