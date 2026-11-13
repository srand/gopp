// The log package: a simple logger that writes lines with a time stamp.
//
//     log::Println("listening on {}", addr);   // 2009/11/10 23:00:00 listening on :8080
//     log::SetFlags(log::LstdFlags | log::Lshortfile);
//     auto l = log::New(ptr<io::Writer>(&os::Stderr), "db: ", log::LstdFlags);
//     l->Print("connected to {}", dsn);
//     log::Fatal("open {}: {}", name, err);          // and os::Exit(1)
//
// The messages are std::format strings, as fmt::Print's are. Print and
// Println write the same: a newline is appended if the message lacks one.
// The file and line (Lshortfile, Llongfile) are where the call is, from
// std::source_location; Output's calldepth is accepted and not needed.
// Panic and Panicln write the message and then panic, which stops the
// program, as gopp::panic does.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>

#include <atomic>
#include <concepts>
#include <format>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#endif

namespace gopp::log {

// The flags that say what each line starts with, before the message:
// Ldate | Ltime gives "2009/01/23 01:23:23 message".
inline constexpr int Ldate = 1 << 0;          // the date in the local time zone: 2009/01/23
inline constexpr int Ltime = 1 << 1;          // the time in the local time zone: 01:23:23
inline constexpr int Lmicroseconds = 1 << 2;  // microsecond resolution: 01:23:23.123123; implies Ltime
inline constexpr int Llongfile = 1 << 3;      // full file name and line number: /a/b/c/d.cpp:23
inline constexpr int Lshortfile = 1 << 4;     // final file name element and line number: d.cpp:23; overrides Llongfile
inline constexpr int LUTC = 1 << 5;           // with Ldate or Ltime, use UTC rather than the local time zone
inline constexpr int Lmsgprefix = 1 << 6;     // move the prefix from the beginning of the line to before the message
inline constexpr int LstdFlags = Ldate | Ltime;  // the standard logger's

// A std::format string and where it was written: the parameter type of
// the logging functions, so that they know their caller's file and line.
template <typename... A>
struct basic_format_string {
    template <typename S>
        requires std::convertible_to<const S&, std::string_view>
    consteval basic_format_string(const S& s, std::source_location l = std::source_location::current())
        : fmt(s), loc(l) {}

    std::format_string<A...> fmt;
    std::source_location loc;
};

template <typename... A>
using format_string = basic_format_string<std::type_identity_t<A>...>;

namespace detail {
// A Writer that also takes the location of the call that logged: slog's
// handler writer, which log::Logger hands the location to.
struct located_writer : io::Writer {
    virtual std::pair<isize, error> WriteLocated(std::span<const byte> p, const std::source_location& loc) = 0;
};
}  // namespace detail

// log.Logger: writes each message as a line to its Writer, with a header
// the flags choose. Safe to use from many fibers at once.
class Logger : public object {
public:
    // A Logger with no Writer: set one with SetOutput before logging.
    Logger() = default;
    Logger(ptr<io::Writer> out, string prefix, int flag);

    // Print and Println format the message and write it as a line.
    template <typename... A>
    void Print(format_string<A...> f, A&&... a) {
        if (discard()) return;
        output(f.loc, std::format(f.fmt, std::forward<A>(a)...));
    }
    void Print(std::source_location loc = std::source_location::current()) { output(loc, {}); }
    template <typename... A>
    void Println(format_string<A...> f, A&&... a) {
        if (discard()) return;
        output(f.loc, std::format(f.fmt, std::forward<A>(a)...));
    }
    void Println(std::source_location loc = std::source_location::current()) { output(loc, {}); }

    // Fatal and Fatalln are Print followed by os::Exit(1).
    template <typename... A>
    [[noreturn]] void Fatal(format_string<A...> f, A&&... a) {
        output(f.loc, std::format(f.fmt, std::forward<A>(a)...));
        exit();
    }
    [[noreturn]] void Fatal(std::source_location loc = std::source_location::current()) {
        output(loc, {});
        exit();
    }
    template <typename... A>
    [[noreturn]] void Fatalln(format_string<A...> f, A&&... a) {
        output(f.loc, std::format(f.fmt, std::forward<A>(a)...));
        exit();
    }
    [[noreturn]] void Fatalln(std::source_location loc = std::source_location::current()) {
        output(loc, {});
        exit();
    }

    // Panic and Panicln are Print followed by a panic with the message.
    template <typename... A>
    [[noreturn]] void Panic(format_string<A...> f, A&&... a) {
        std::string s = std::format(f.fmt, std::forward<A>(a)...);
        output(f.loc, s);
        gopp::panic(s);
    }
    template <typename... A>
    [[noreturn]] void Panicln(format_string<A...> f, A&&... a) {
        std::string s = std::format(f.fmt, std::forward<A>(a)...);
        s.push_back('\n');
        output(f.loc, s);
        gopp::panic(s);
    }

    // Writes s as a line. The file and line are those of the caller;
    // calldepth, which says which caller in Go, is ignored.
    error Output(int calldepth, std::string_view s, std::source_location loc = std::source_location::current()) {
        return output(loc, s);
    }

    void SetOutput(ptr<io::Writer> w);
    ptr<io::Writer> Writer() const;
    int Flags() const noexcept { return flag_.load(std::memory_order_relaxed); }
    void SetFlags(int flag) noexcept { flag_.store(flag, std::memory_order_relaxed); }
    string Prefix() const;
    void SetPrefix(string prefix);

    // Writes s with the header for loc; the core of the functions above.
    error output(const std::source_location& loc, std::string_view s);

private:
    bool discard() const noexcept { return is_discard_.load(std::memory_order_relaxed); }
    [[noreturn]] static void exit();

    mutable sync::Mutex out_mu_;
    ptr<io::Writer> out_;
    std::atomic<std::shared_ptr<const string>> prefix_;
    std::atomic<int> flag_{0};
    std::atomic<bool> is_discard_{false};
};

// log.New: a Logger writing to out. prefix starts each line, or comes
// before the message with Lmsgprefix.
ptr<Logger> New(ptr<io::Writer> out, string prefix, int flag);

// log.Default: the standard logger, which the package functions use. It
// writes to os::Stderr with LstdFlags.
ptr<Logger> Default();

// The standard logger's settings.
void SetOutput(ptr<io::Writer> w);
ptr<io::Writer> Writer();
int Flags();
void SetFlags(int flag);
string Prefix();
void SetPrefix(string prefix);

// The standard logger's output functions.
template <typename... A>
void Print(format_string<A...> f, A&&... a) {
    Default()->Print(f, std::forward<A>(a)...);
}
inline void Print(std::source_location loc = std::source_location::current()) { Default()->Print(loc); }
template <typename... A>
void Println(format_string<A...> f, A&&... a) {
    Default()->Println(f, std::forward<A>(a)...);
}
inline void Println(std::source_location loc = std::source_location::current()) { Default()->Println(loc); }
template <typename... A>
[[noreturn]] void Fatal(format_string<A...> f, A&&... a) {
    Default()->Fatal(f, std::forward<A>(a)...);
}
[[noreturn]] inline void Fatal(std::source_location loc = std::source_location::current()) { Default()->Fatal(loc); }
template <typename... A>
[[noreturn]] void Fatalln(format_string<A...> f, A&&... a) {
    Default()->Fatalln(f, std::forward<A>(a)...);
}
[[noreturn]] inline void Fatalln(std::source_location loc = std::source_location::current()) {
    Default()->Fatalln(loc);
}
template <typename... A>
[[noreturn]] void Panic(format_string<A...> f, A&&... a) {
    Default()->Panic(f, std::forward<A>(a)...);
}
template <typename... A>
[[noreturn]] void Panicln(format_string<A...> f, A&&... a) {
    Default()->Panicln(f, std::forward<A>(a)...);
}
inline error Output(int calldepth, std::string_view s, std::source_location loc = std::source_location::current()) {
    return Default()->Output(calldepth, s, loc);
}

}  // namespace gopp::log
