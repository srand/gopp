// The net/url package (the parts HTTP servers need).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::url {

// url.Values: query parameters, in order, with repeated keys.
class Values {
public:
    string Get(std::string_view key) const;
    void Set(string key, string value);
    void Add(string key, string value);
    void Del(std::string_view key);
    bool Has(std::string_view key) const;
    std::size_t size() const noexcept { return kv_.size(); }
    // Encodes as "a=1&b=2", sorted by key.
    string Encode() const;

    auto begin() const noexcept { return kv_.begin(); }
    auto end() const noexcept { return kv_.end(); }

private:
    std::vector<std::pair<string, string>> kv_;
};

// url.URL
struct URL {
    string Scheme;
    string Opaque;
    string Host;       // host or host:port
    string Path;       // decoded
    string RawPath;   // encoded path hint, empty if path encodes as is
    string RawQuery;  // without '?'
    string Fragment;

    // The escaped form of path.
    string EscapedPath() const;
    // The path and query, as sent in an HTTP request line.
    string RequestURI() const;
    // The parsed RawQuery; malformed pairs are dropped.
    Values Query() const;
    // The URL as a string.
    string String() const;
    // Host without the port.
    string Hostname() const;
    string Port() const;
};

// url.Error
namespace detail {
// Implements Error() for an error type named Error, which can't declare it.
template <typename E>
struct named_error : errorInterface {
    string Error() const override { return static_cast<const E&>(*this).message(); }
};
}  // namespace detail

struct Error : detail::named_error<Error> {
    string Op;
    string URL;
    gopp::error Err;
    // The message, as Error() returns it.
    string message() const;
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
    bool Timeout() const noexcept override { return Err && Err->Timeout(); }
};

// url.Parse
std::pair<URL, error> Parse(std::string_view rawurl);
// url.ParseRequestURI: an absolute URI or an absolute path, without fragment.
std::pair<URL, error> ParseRequestURI(std::string_view rawurl);
// url.ParseQuery
std::pair<Values, error> ParseQuery(std::string_view query);

// url.PathUnescape / url.QueryUnescape
std::pair<string, error> PathUnescape(std::string_view s);
std::pair<string, error> QueryUnescape(std::string_view s);
// url.PathEscape / url.QueryEscape
string PathEscape(std::string_view s);
string QueryEscape(std::string_view s);

}  // namespace gopp::url
