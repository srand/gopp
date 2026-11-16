// The net/http/cgi package, host side: runs a CGI program for each
// request. Ported from Go 1.26's net/http/cgi/host.go.
//
//     auto h = make_ptr<cgi::Handler>();
//     h->Path = "/usr/lib/git-core/git-http-backend";
//     h->Root = "/git";
//     h->Env = {"GIT_PROJECT_ROOT=/srv/git", "GIT_HTTP_EXPORT_ALL=1"};
//     http::Handle("/git/", h);
//
// The request body is the program's standard input, and its standard
// output is the response: after the headers it is copied as it arrives, and
// flushed after each read, so a response larger than memory streams. If the
// client goes away, the program is killed and waited for.
//
// Differences from Go: AllowChunked (below); the program is also killed
// when its headers are bad, not only after a copy error (after an internal
// redirect it is not killed: it is waited for, as in Go); and an internal
// redirect takes the request's scheme and host without cleaning dot
// segments out of the path (go++ has no URL resolution).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/net/http.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#endif

namespace gopp::cgi {

// cgi.Handler: runs an executable in a subprocess with a CGI environment.
struct Handler final : http::Handler {
    // The CGI executable.
    string Path;
    // The URL prefix the handler is at, or "" for "/".
    string Root;
    // The working directory of the program. If empty, the directory of
    // Path; if Path has none, the current directory.
    string Dir;

    // Extra environment variables to set, as "key=value".
    slice<string> Env;
    // Names of this process's environment variables to pass on.
    slice<string> InheritEnv;
    // Where errors are logged; log::Default() if nil.
    ptr<log::Logger> Logger;
    // Extra arguments for the program.
    slice<string> Args;
    // The program's standard error; os::Stderr if nil.
    ptr<io::Writer> Stderr;

    // The handler of internal redirects, which the program asks for with a
    // Location header that starts with "/" (RFC 3875, 6.3.2); the redirect
    // is handled as a GET. If nil, such a Location goes to the client (with
    // status 302 if the program gave none), as in Go.
    ptr<http::Handler> PathLocationHandler;

    // go++ addition (not in Go): CGI forbids chunked request bodies, and
    // Go answers them with 400. With AllowChunked, a chunked body streams
    // to the program's standard input with CONTENT_LENGTH unset, and the
    // program reads to the end of its input (as git http-backend does).
    bool AllowChunked = false;

    void ServeHTTP(http::ResponseWriter& w, http::Request& r) override;

private:
    template <typename... A>
    void printf(std::format_string<A...> f, A&&... a);
    void handleInternalRedirect(http::ResponseWriter& w, http::Request& r, std::string_view path);
};

namespace detail {
// Removes the entries of env whose key comes again later: the last one
// counts. Exported for the tests (Go's removeLeadingDuplicates).
slice<string> removeLeadingDuplicates(const slice<string>& env);
}  // namespace detail

}  // namespace gopp::cgi
