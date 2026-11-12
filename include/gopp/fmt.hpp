// The fmt package, with std::format's "{}" syntax instead of Go's verbs.
//
//     fmt::Print(w, "data: {}\n\n", event);   // fmt.Fprintf(w, ...)
//     fmt::Println("listening on {}", addr);  // fmt.Printf(... + "\n")
//     auto s = fmt::Sprint("{}:{}", host, port);
//     return fmt::Errorf("bad port {}", port);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/string.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#endif

namespace gopp::fmt {

namespace detail {
void write_stdout(std::string_view s);
}

// Arguments are forwarded, never moved from: formatting only reads them.

// Formats into w (fmt.Fprintf).
template <typename... A>
std::pair<isize, error> Print(io::Writer& w, std::format_string<A...> f, A&&... a) {
    char buf[512];
    auto r = std::format_to_n(buf, sizeof(buf), f, std::forward<A>(a)...);
    if (static_cast<std::size_t>(r.size) <= sizeof(buf)) {
        return io::WriteString(w, std::string_view(buf, static_cast<std::size_t>(r.size)));
    }
    std::string s = std::format(f, std::forward<A>(a)...);
    return io::WriteString(w, s);
}

// Formats into w and appends a newline (fmt.Fprintln).
template <typename... A>
std::pair<isize, error> Println(io::Writer& w, std::format_string<A...> f, A&&... a) {
    char buf[512];
    auto r = std::format_to_n(buf, sizeof(buf) - 1, f, std::forward<A>(a)...);
    if (static_cast<std::size_t>(r.size) <= sizeof(buf) - 1) {
        buf[r.size] = '\n';
        return io::WriteString(w, std::string_view(buf, static_cast<std::size_t>(r.size) + 1));
    }
    std::string s = std::format(f, std::forward<A>(a)...);
    s.push_back('\n');
    return io::WriteString(w, s);
}

// Formats to standard output (fmt.Printf).
template <typename... A>
void Print(std::format_string<A...> f, A&&... a) {
    detail::write_stdout(std::format(f, std::forward<A>(a)...));
}

// Formats to standard output and appends a newline.
template <typename... A>
void Println(std::format_string<A...> f, A&&... a) {
    std::string s = std::format(f, std::forward<A>(a)...);
    s.push_back('\n');
    detail::write_stdout(s);
}

// Formats into a string (fmt.Sprintf).
template <typename... A>
string Sprint(std::format_string<A...> f, A&&... a) {
    return string(std::format(f, std::forward<A>(a)...));
}

// An error with a formatted message (fmt.Errorf).
template <typename... A>
error Errorf(std::format_string<A...> f, A&&... a) {
    return errors::New(Sprint(f, std::forward<A>(a)...));
}

}  // namespace gopp::fmt
