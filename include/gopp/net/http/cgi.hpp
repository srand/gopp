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
    // is handled as a GET. http::DefaultServeMux() if nil.
    ptr<http::Handler> PathLocationHandler;

    void ServeHTTP(http::ResponseWriter& w, http::Request& r) override;

private:
    template <typename... A>
    void printf(std::format_string<A...> f, A&&... a);
    void handleInternalRedirect(http::ResponseWriter& w, http::Request& r, std::string_view path);
};

// Removes the entries of env whose key comes again later: the last one
// counts. Exported for the tests (Go's removeLeadingDuplicates).
slice<string> removeLeadingDuplicates(const slice<string>& env);

}  // namespace gopp::cgi
