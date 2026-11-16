// The host side of CGI, ported from Go 1.26's net/http/cgi/host.go.
#include "net/http/internal.hpp"

#include <gopp/bufio.hpp>
#include <gopp/net.hpp>
#include <gopp/net/http/cgi.hpp>
#include <gopp/net/textproto.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/strings.hpp>

#include <format>
#include <vector>

namespace gopp::cgi {

namespace {

// Go's trailingPort, `:([0-9]+)$`: the digits after the last colon, if
// there are some and nothing else.
std::string_view trailingPort(std::string_view host) {
    auto colon = host.rfind(':');
    if (colon == std::string_view::npos) return {};
    auto digits = host.substr(colon + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos) return {};
    return digits;
}

// The environment variables that a CGI program gets from the host's.
std::vector<std::string_view> osDefaultInheritEnv() { return {"LD_LIBRARY_PATH"}; }

// Go's upperCaseAndUnderscore, mapped over a header key.
std::string upperCaseAndUnderscore(std::string_view k) {
    std::string r;
    r.reserve(k.size());
    for (char c : k) {
        if (c >= 'a' && c <= 'z') r.push_back(char(c - ('a' - 'A')));
        else if (c == '-') r.push_back('_');
        else if (c == '=') continue;  // it would mess up the "key=value" environment
        else r.push_back(c);
    }
    return r;
}

// Kills the program unless it ended on its own, then closes its output and
// waits for it, so that the handler never returns with the process running
// or unreaped. (Go kills only after a copy error: here also after the other
// ways out, since a program that has not been read from may block forever.)
struct reaper {
    ptr<os::exec::Cmd> cmd;
    ptr<os::File> stdout_;
    bool killed = false;
    bool finished = false;  // the program's output ended

    void kill() {
        if (killed || finished) return;
        killed = true;
        cmd->Process->Kill();
    }
    ~reaper() {
        if (!finished) kill();
        stdout_->Close();
        cmd->Wait();
    }
};

}  // namespace

slice<string> removeLeadingDuplicates(const slice<string>& env) {
    slice<string> ret;
    for (std::size_t i = 0; i < env.size(); i++) {
        bool found = false;
        std::string_view e = env[i].view();
        if (auto eq = e.find('='); eq != std::string_view::npos) {
            std::string_view keq = e.substr(0, eq + 1);  // "key="
            for (std::size_t j = i + 1; j < env.size(); j++) {
                if (env[j].view().starts_with(keq)) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) ret = append(ret, env[i]);
    }
    return ret;
}

template <typename... A>
void Handler::printf(std::format_string<A...> f, A&&... a) {
    auto l = Logger ? Logger : log::Default();
    l->Print("{}", std::format(f, std::forward<A>(a)...));
}

void Handler::ServeHTTP(http::ResponseWriter& rw, http::Request& req) {
    if (!req.TransferEncoding.empty() && req.TransferEncoding[0] == "chunked") {
        rw.WriteHeader(http::StatusBadRequest);
        io::WriteString(rw, "Chunked request bodies are not supported by CGI.");
        return;
    }

    std::string root = std::string(strings::TrimRight(Root, "/").view());
    std::string pathInfo = std::string(strings::TrimPrefix(req.URL.Path, root).view());

    std::string port = "80";
    if (req.TLS) port = "443";
    if (auto p = trailingPort(req.Host); !p.empty()) port = std::string(p);

    std::vector<string> env = {
        "SERVER_SOFTWARE=go",
        "SERVER_PROTOCOL=HTTP/1.1",
        "HTTP_HOST=" + req.Host,
        "GATEWAY_INTERFACE=CGI/1.1",
        "REQUEST_METHOD=" + req.Method,
        "QUERY_STRING=" + req.URL.RawQuery,
        "REQUEST_URI=" + req.URL.RequestURI(),
        string("PATH_INFO=" + pathInfo),
        string("SCRIPT_NAME=" + root),
        "SCRIPT_FILENAME=" + Path,
        string("SERVER_PORT=" + port),
    };

    if (auto [remoteIP, remotePort, err] = net::SplitHostPort(req.RemoteAddr); !err) {
        env.push_back("REMOTE_ADDR=" + remoteIP);
        env.push_back("REMOTE_HOST=" + remoteIP);
        env.push_back("REMOTE_PORT=" + remotePort);
    } else {
        // could not parse ip:port, let's use whole RemoteAddr and leave REMOTE_PORT undefined
        env.push_back("REMOTE_ADDR=" + req.RemoteAddr);
        env.push_back("REMOTE_HOST=" + req.RemoteAddr);
    }

    if (auto [hostDomain, hostPort, err] = net::SplitHostPort(req.Host); !err) env.push_back("SERVER_NAME=" + hostDomain);
    else env.push_back("SERVER_NAME=" + req.Host);

    if (req.TLS) env.push_back("HTTPS=on");

    // The header keys, each once, in the order they came.
    std::vector<string> seen;
    for (auto& e : req.Header) {
        bool dup = false;
        for (auto& s : seen) dup = dup || s == e.key;
        if (!dup) seen.push_back(e.key);
    }
    for (auto& key : seen) {
        std::string k = upperCaseAndUnderscore(key.view());
        if (k == "PROXY") {
            // See Issue 16405
            continue;
        }
        std::string_view joinStr = k == "COOKIE" ? "; " : ", ";
        std::string v;
        auto vals = req.Header.Values(key.view());
        for (std::size_t i = 0; i < vals.size(); i++) {
            if (i) v += joinStr;
            v += vals[i].view();
        }
        env.push_back(string("HTTP_" + k + "=" + v));
    }

    if (req.ContentLength > 0) env.push_back(string(std::format("CONTENT_LENGTH={}", req.ContentLength)));
    if (auto ctype = req.Header.Get("Content-Type"); !ctype.empty()) env.push_back("CONTENT_TYPE=" + ctype);

    string envPath = os::Getenv("PATH");
    if (envPath.empty()) envPath = "/bin:/usr/bin:/usr/ucb:/usr/bsd:/usr/local/bin";
    env.push_back("PATH=" + envPath);

    for (auto& e : InheritEnv) {
        if (auto v = os::Getenv(e.view()); !v.empty()) env.push_back(e + "=" + v);
    }
    for (auto e : osDefaultInheritEnv()) {
        if (auto v = os::Getenv(e); !v.empty()) env.push_back(string(e) + "=" + v);
    }
    for (auto& e : Env) env.push_back(e);

    auto cleanEnv = removeLeadingDuplicates(slice<string>::of(env));

    string cwd, path;
    if (!Dir.empty()) {
        path = Path;
        cwd = Dir;
    } else {
        std::tie(cwd, path) = filepath::Split(Path.view());
    }
    if (cwd.empty()) cwd = ".";

    auto internalError = [&](const error& err) {
        rw.WriteHeader(http::StatusInternalServerError);
        printf("CGI error: {}", err);
    };

    auto cmd = make_ptr<os::exec::Cmd>();
    cmd->Path = path;
    cmd->Args = append(slice<string>(), Path);
    cmd->Args = appendSpan(cmd->Args, std::span<const string>(Args.begin(), Args.size()));
    cmd->Dir = cwd;
    cmd->Env = cleanEnv;
    cmd->Stderr = Stderr ? Stderr : ptr<io::Writer>(&os::Stderr);
    if (req.ContentLength != 0) cmd->Stdin = req.Body;
    auto [stdoutRead, err] = cmd->StdoutPipe();
    if (err) {
        internalError(err);
        return;
    }

    if ((err = cmd->Start())) {
        internalError(err);
        return;
    }
    reaper guard{cmd, stdoutRead};

    auto linebody = make_ptr<bufio::Reader>(stdoutRead, 1024);
    http::Header headers;
    int statusCode = 0;
    int headerLines = 0;
    bool sawBlankLine = false;
    for (;;) {
        auto [line, isPrefix, rerr] = linebody->ReadLine();
        if (isPrefix) {
            rw.WriteHeader(http::StatusInternalServerError);
            printf("cgi: long header line from subprocess.");
            return;
        }
        if (rerr == io::EOF_) break;
        if (rerr) {
            rw.WriteHeader(http::StatusInternalServerError);
            printf("cgi: error reading headers: {}", rerr);
            return;
        }
        if (line.empty()) {
            sawBlankLine = true;
            break;
        }
        headerLines++;
        std::string_view l(reinterpret_cast<const char*>(line.data()), line.size());
        auto colon = l.find(':');
        if (colon == std::string_view::npos) {
            printf("cgi: bogus header line: {}", l);
            continue;
        }
        std::string_view header = l.substr(0, colon);
        if (!http::internal::validHeaderFieldName(header)) {
            printf("cgi: invalid header name: \"{}\"", header);
            continue;
        }
        string val = textproto::TrimString(string(l.substr(colon + 1)));
        if (header == "Status") {
            if (val.size() < 3) {
                printf("cgi: bogus status (short): \"{}\"", val.view());
                return;
            }
            auto [code, aerr] = strconv::Atoi(val.view().substr(0, 3));
            if (aerr) {
                printf("cgi: bogus status: \"{}\"", val.view());
                printf("cgi: line was \"{}\"", l);
                return;
            }
            statusCode = int(code);
        } else {
            headers.Add(header, val);
        }
    }
    if (headerLines == 0 || !sawBlankLine) {
        rw.WriteHeader(http::StatusInternalServerError);
        printf("cgi: no headers");
        return;
    }

    if (auto loc = headers.Get("Location"); !loc.empty()) {
        if (loc.starts_with("/")) {
            handleInternalRedirect(rw, req, loc.view());
            return;
        }
        if (statusCode == 0) statusCode = http::StatusFound;
    }

    if (statusCode == 0 && headers.Get("Content-Type").empty()) {
        rw.WriteHeader(http::StatusInternalServerError);
        printf("cgi: missing required Content-Type in headers");
        return;
    }

    if (statusCode == 0) statusCode = http::StatusOK;

    // Copy headers to rw's headers, after we've decided not to
    // go into handleInternalRedirect, which won't want its rw
    // headers to have been touched.
    for (auto& e : headers) rw.Header().Add(e.key.view(), e.value);

    rw.WriteHeader(statusCode);

    // The body: what the program writes goes to the client as it comes,
    // flushed after each read (as git's progress messages need).
    auto* flusher = dynamic_cast<http::Flusher*>(&rw);
    std::vector<byte> buf(32 << 10);
    error copyErr;
    for (;;) {
        auto [n, rerr] = linebody->Read(buf);
        if (n > 0) {
            auto [wn, werr] = rw.Write(std::span<const byte>(buf.data(), std::size_t(n)));
            if (!werr && wn < n) werr = io::ErrShortWrite;
            if (werr) {
                copyErr = werr;
                break;
            }
            if (flusher) flusher->Flush();
        }
        if (rerr) {
            if (rerr != io::EOF_) copyErr = rerr;
            else guard.finished = true;
            break;
        }
    }
    if (copyErr) {
        printf("cgi: copy error: {}", copyErr);
        // The guard kills the program, so that we don't hang in the Wait
        // if the error was just the client (rw) going away. If it was a
        // read error (because the program died itself), then the extra
        // kill of an already-dead process is harmless (the PID won't be
        // reused until the Wait).
    }
}

void Handler::handleInternalRedirect(http::ResponseWriter& rw, http::Request& req, std::string_view path) {
    // url := req.URL.Parse(path): path is an absolute path, so it takes the
    // scheme and host of the request's URL.
    auto [u, err] = url::Parse(path);
    if (err) {
        rw.WriteHeader(http::StatusInternalServerError);
        printf("cgi: error resolving local URI path \"{}\": {}", path, err);
        return;
    }
    if (u.Scheme.empty() && u.Host.empty()) {
        u.Scheme = req.URL.Scheme;
        u.Host = req.URL.Host;
    }
    // TODO: RFC 3875 isn't clear if only GET is supported, but it
    // suggests so: "Note that any message-body attached to the
    // request (such as for a POST request) may not be available
    // to the resource that is the target of the redirect."  We
    // should do some tests against Apache to see how it handles
    // POST, HEAD, etc. Does the internal redirect get the same
    // method or just GET? What about incoming headers?
    // (e.g. Cookies) Which headers, if any, are copied into the
    // second request?
    http::Request newReq;
    newReq.Method = "GET";
    newReq.URL = std::move(u);
    newReq.Proto = "HTTP/1.1";
    newReq.ProtoMajor = 1;
    newReq.ProtoMinor = 1;
    newReq.Host = newReq.URL.Host;
    newReq.RemoteAddr = req.RemoteAddr;
    newReq.TLS = req.TLS;
    newReq.Body = io::NopCloser(strings::NewReader(string()));
    http::Handler* h = PathLocationHandler ? PathLocationHandler.get() : &http::DefaultServeMux();
    h->ServeHTTP(rw, newReq);
}

}  // namespace gopp::cgi
