// The fmt package, with std::format's "{}" syntax instead of Go's verbs.
//
//     fmt::Print(w, "data: {}\n\n", event);   // fmt.Fprintf(w, ...)
//     fmt::Println("listening on {}", addr);  // fmt.Printf(... + "\n")
//     auto s = fmt::Sprint("{}:{}", host, port);
//     return fmt::Errorf("bad port {}", port);
//     return fmt::Errorf("open {}: {}", name, fmt::W(err));   // %w: wraps err
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
#include <vector>
#endif

namespace gopp::fmt {

// Go's %w verb: an Errorf argument that formats as the error does, and
// that the error Errorf returns wraps (errors::Is and As find it). With
// several, the error wraps them all, as Go's Unwrap() []error.
struct W {
    W(gopp::error e) noexcept : err(std::move(e)) {}
    gopp::error err;
};

namespace detail {
void write_stdout(std::string_view s);
// The error of Errorf: msg, wrapping the errors of its fmt::W arguments.
error wrap_errors(string msg, std::vector<gopp::error> wrapped);
inline void collect_wrapped(std::vector<gopp::error>& v, const W& w) { v.push_back(w.err); }
template <typename T>
void collect_wrapped(std::vector<gopp::error>&, const T&) {}
}  // namespace detail

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

// An error with a formatted message (fmt.Errorf); fmt::W arguments are
// wrapped, as %w wraps in Go.
template <typename... A>
error Errorf(std::format_string<A...> f, A&&... a) {
    constexpr bool wraps = (std::is_same_v<std::remove_cvref_t<A>, W> || ...);
    if constexpr (wraps) {
        std::vector<gopp::error> wrapped;
        (detail::collect_wrapped(wrapped, a), ...);
        return detail::wrap_errors(Sprint(f, std::forward<A>(a)...), std::move(wrapped));
    } else {
        return errors::New(Sprint(f, std::forward<A>(a)...));
    }
}

}  // namespace gopp::fmt

template <>
struct std::formatter<gopp::fmt::W> : std::formatter<std::string_view> {
    auto format(const gopp::fmt::W& w, auto& ctx) const {
        return std::formatter<std::string_view>::format(w.err.Error().view(), ctx);
    }
};
