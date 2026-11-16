// Ported from Go 1.26's net/http/cgi/host_test.go (and cgi_main.go's
// testCGI), with Go's test names. Go runs a CGI program that is the test
// binary itself, started by TestMain; here it is the helper command
// "cgi-test" of go_helper.hpp, which does what Go's testCGI does. Left out:
// handlerRunning (a goroutine dump) in TestCopyError, which tracks the
// running handler with a counter instead; and the tests of the child side
// (child_test.go and integration_test.go), which go++ doesn't have.
// Deviations of the handler from Go: AllowChunked (CGIPostChunkedAllowed),
// killing the program on bad headers (but not after an internal redirect,
// where it is waited for: InternalRedirectWaitsForChild), and no
// dot-segment cleaning in an internal redirect.
// Added: CGIStreamsLargeBody (a response of 256 MiB passes through with
// the memory of the test unchanged) and CGIClientDisconnectKillsChild.
#include "go_helper.hpp"

#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/net.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/cgi.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/net/url.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <map>

using namespace gopp;

extern char** environ;

namespace {

// ---------------------------------------------------------------------------
// The CGI program: Go's testCGI.

void writeAll(const char* p, std::size_t n) {
    while (n > 0) {
        ssize_t w = ::write(1, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::_exit(0);  // the reader went away
        }
        p += w;
        n -= std::size_t(w);
    }
}

void out(const std::string& s) { writeAll(s.data(), s.size()); }

std::string getenvStr(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : "";
}

// The first value of each key of the query and of a form body, which
// Go's Request().ParseForm gives.
std::map<std::string, std::string> parseForm() {
    std::map<std::string, std::string> params;
    auto add = [&](std::string_view q) {
        auto [vals, err] = url::ParseQuery(q);
        for (auto& [k, v] : vals) params.emplace(std::string(k.view()), std::string(v.view()));
    };
    std::string body;
    if (getenvStr("REQUEST_METHOD") == "POST") {
        std::size_t n = std::strtoull(getenvStr("CONTENT_LENGTH").c_str(), nullptr, 10);
        body.resize(n);
        std::size_t got = 0;
        while (got < n) {
            ssize_t r = ::read(0, body.data() + got, n - got);
            if (r <= 0) break;
            got += std::size_t(r);
        }
        body.resize(got);
        add(body);  // body fields first
    }
    add(getenvStr("QUERY_STRING"));
    return params;
}

void cmdCgiTest(const std::vector<std::string>&) {
    auto params = parseForm();
    if (auto it = params.find("loc"); it != params.end() && !it->second.empty()) {
        out("Location: " + it->second + "\r\n\r\n");
        if (auto m = params.find("marker"); m != params.end()) {
            // Work done after the response: it must not be cut short.
            ::close(1);
            ::usleep(300 * 1000);
            if (FILE* f = std::fopen(m->second.c_str(), "w")) std::fclose(f);
        }
        return;
    }
    out("Content-Type: text/html\r\nX-CGI-Pid: " + std::to_string(::getpid()) + "\r\nX-Test-Header: X-Test-Value\r\n\r\n");
    if (params.count("writestderr")) {
        const char msg[] = "Hello, stderr!\n";
        if (::write(2, msg, sizeof msg - 1) < 0) {}
    }
    if (params.count("bigresponse")) {
        // 17 MB, for OS X: golang.org/issue/4958
        std::string line(1024, 'A');
        line += "\r\n";
        for (int i = 0; i < 17 * 1024; i++) out(line);
        return;
    }
    if (auto it = params.find("stream"); it != params.end()) {
        std::size_t total = std::strtoull(it->second.c_str(), nullptr, 10);
        std::string chunk(64 << 10, 'x');
        for (std::size_t sent = 0; sent < total;) {
            std::size_t n = std::min(chunk.size(), total - sent);
            writeAll(chunk.data(), n);
            sent += n;
        }
        return;
    }
    if (params.count("forever")) {
        std::string chunk(64 << 10, 'y');
        for (;;) writeAll(chunk.data(), chunk.size());
    }
    if (params.count("sum")) {
        // The whole of standard input: its length and FNV-1a hash.
        std::uint64_t h = 14695981039346656037ull, n = 0;
        char b[65536];
        for (;;) {
            ssize_t r = ::read(0, b, sizeof b);
            if (r <= 0) break;
            for (ssize_t i = 0; i < r; i++) h = (h ^ std::uint8_t(b[i])) * 1099511628211ull;
            n += std::uint64_t(r);
        }
        out("bytes=" + std::to_string(n) + "\r\nsum=" + std::to_string(h) + "\r\n");
        return;
    }
    out("test=Hello CGI\r\n");
    for (auto& [k, v] : params) out("param-" + k + "=" + v + "\r\n");
    std::map<std::string, std::string> envs;
    for (char** e = environ; e && *e; e++) {
        std::string kv = *e;
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        envs[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (auto& [k, v] : envs) out("env-" + k + "=" + v + "\r\n");
    auto [cwd, err] = os::Getwd();
    out("cwd=" + std::string(cwd.view()) + "\r\n");
}

const bool helpersRegistered = go_helper::registerHelper("cgi-test", cmdCgiTest);

// ---------------------------------------------------------------------------
// Helpers.

std::string exe() { return std::string(go_helper::executable().view()); }

// A handler running the test program as the CGI program. Its environment has
// what go_helper needs to run the helper command.
ptr<cgi::Handler> newHandler(std::string root = "", slice<string> env = {}) {
    auto h = make_ptr<cgi::Handler>();
    h->Path = string(exe());
    h->Root = string(root);
    h->Args = {"cgi-test"};
    h->Env = append(env, string("GO_EXEC_TEST_PID=" + std::to_string(::getpid())));
    return h;
}

// Go's newRequest: a request read from the text of an HTTP/1 request. The
// lines end with "\n".
ptr<http::Request> newRequest(const std::string& httpreq) {
    auto lines = std::vector<std::string>();
    std::size_t pos = 0;
    std::string body;
    bool inBody = false;
    while (pos < httpreq.size()) {
        auto nl = httpreq.find('\n', pos);
        std::string line = httpreq.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? httpreq.size() : nl + 1;
        if (line.empty() && !inBody) {
            inBody = true;
            body = httpreq.substr(pos);
            break;
        }
        lines.push_back(line);
    }
    auto sp1 = lines.at(0).find(' ');
    auto sp2 = lines.at(0).find(' ', sp1 + 1);
    std::string method = lines[0].substr(0, sp1), target = lines[0].substr(sp1 + 1, sp2 - sp1 - 1);
    auto req = httptest::NewRequest(method, target, strings::NewReader(string(body)));
    req->Host = "";
    req->ContentLength = 0;
    for (std::size_t i = 1; i < lines.size(); i++) {
        auto colon = lines[i].find(':');
        EXPECT_NE(colon, std::string::npos) << "cgi: bogus http request in test: " << httpreq;
        std::string k = lines[i].substr(0, colon), v = lines[i].substr(colon + 1);
        v.erase(0, v.find_first_not_of(' '));
        if (http::CanonicalHeaderKey(k) == "Host") req->Host = string(v);
        else if (http::CanonicalHeaderKey(k) == "Content-Length") req->ContentLength = std::atoll(v.c_str());
        else if (http::CanonicalHeaderKey(k) == "Transfer-Encoding") {
            req->TransferEncoding = {string(v)};
            req->ContentLength = -1;
        } else req->Header.Add(k, string(v));
    }
    req->RemoteAddr = "1.2.3.4:1234";
    return req;
}

using strmap = std::map<std::string, std::string>;
using check = std::function<void(const strmap&)>;

void runResponseChecks(httptest::ResponseRecorder& rw, const strmap& expectedMap,
                       const std::vector<check>& checks = {}) {
    // The map that the CGI returned.
    strmap m;
    m["_body"] = rw.Body;
    std::size_t pos = 0;
    int linesRead = 0;
    while (pos < rw.Body.size()) {
        auto nl = rw.Body.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = rw.Body.substr(pos, nl - pos + 1);
        pos = nl + 1;
        linesRead++;
        std::string trimmed = line.substr(0, line.find_last_not_of("\r\n") + 1);
        auto eq = trimmed.find('=');
        ASSERT_NE(eq, std::string::npos) << "Unexpected response from invalid line number " << linesRead << ": " << line;
        m[trimmed.substr(0, eq)] = trimmed.substr(eq + 1);
    }
    for (auto& [key, expected] : expectedMap) {
        std::string got = m[key];
        if (key == "cwd") {
            auto [fi1, e1] = os::Stat(got);
            auto [fi2, e2] = os::Stat(expected);
            if (fi1 && fi2 && os::SameFile(fi1, fi2)) got = expected;
        }
        EXPECT_EQ(got, expected) << "for key " << key;
    }
    for (auto& c : checks) c(m);
}

ptr<httptest::ResponseRecorder> runCgiTest(cgi::Handler& h, const std::string& httpreq, const strmap& expectedMap,
                                           const std::vector<check>& checks = {}) {
    auto rw = httptest::NewRecorder();
    auto req = newRequest(httpreq);
    h.ServeHTTP(*rw, *req);
    runResponseChecks(*rw, expectedMap, checks);
    return rw;
}

std::string chunk(const std::string& s) { return std::format("{:x}\r\n{}\r\n", s.size(), s); }

// A server, closed when it goes out of scope.
struct server {
    ptr<httptest::Server> ts;
    explicit server(ptr<http::Handler> h) : ts(httptest::NewServer(std::move(h))) {}
    ~server() { ts->Close(); }
    httptest::Server* operator->() const { return ts.get(); }
};

// ---------------------------------------------------------------------------
// Tests.

TEST(GoCGI, CGIBasicGet) {
    auto h = newHandler("/test.cgi");
    strmap expectedMap = {
        {"test", "Hello CGI"},
        {"param-a", "b"},
        {"param-foo", "bar"},
        {"env-GATEWAY_INTERFACE", "CGI/1.1"},
        {"env-HTTP_HOST", "example.com:80"},
        {"env-PATH_INFO", ""},
        {"env-QUERY_STRING", "foo=bar&a=b"},
        {"env-REMOTE_ADDR", "1.2.3.4"},
        {"env-REMOTE_HOST", "1.2.3.4"},
        {"env-REMOTE_PORT", "1234"},
        {"env-REQUEST_METHOD", "GET"},
        {"env-REQUEST_URI", "/test.cgi?foo=bar&a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-SCRIPT_NAME", "/test.cgi"},
        {"env-SERVER_NAME", "example.com"},
        {"env-SERVER_PORT", "80"},
        {"env-SERVER_SOFTWARE", "go"},
    };
    auto replay = runCgiTest(*h, "GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com:80\n\n", expectedMap);
    EXPECT_EQ(replay->Header().Get("Content-Type"), "text/html");
    EXPECT_EQ(replay->Header().Get("X-Test-Header"), "X-Test-Value");
}

TEST(GoCGI, CGIEnvIPv6) {
    auto h = newHandler("/test.cgi");
    strmap expectedMap = {
        {"test", "Hello CGI"},
        {"param-a", "b"},
        {"param-foo", "bar"},
        {"env-GATEWAY_INTERFACE", "CGI/1.1"},
        {"env-HTTP_HOST", "example.com"},
        {"env-PATH_INFO", ""},
        {"env-QUERY_STRING", "foo=bar&a=b"},
        {"env-REMOTE_ADDR", "2000::3000"},
        {"env-REMOTE_HOST", "2000::3000"},
        {"env-REMOTE_PORT", "12345"},
        {"env-REQUEST_METHOD", "GET"},
        {"env-REQUEST_URI", "/test.cgi?foo=bar&a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-SCRIPT_NAME", "/test.cgi"},
        {"env-SERVER_NAME", "example.com"},
        {"env-SERVER_PORT", "80"},
        {"env-SERVER_SOFTWARE", "go"},
    };
    auto rw = httptest::NewRecorder();
    auto req = newRequest("GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com\n\n");
    req->RemoteAddr = "[2000::3000]:12345";
    h->ServeHTTP(*rw, *req);
    runResponseChecks(*rw, expectedMap);
}

TEST(GoCGI, CGIBasicGetAbsPath) {
    auto [absPath, err] = filepath::Abs(exe());
    ASSERT_FALSE(err) << err;
    auto h = newHandler("/test.cgi");
    h->Path = absPath;
    strmap expectedMap = {
        {"env-REQUEST_URI", "/test.cgi?foo=bar&a=b"},
        {"env-SCRIPT_FILENAME", std::string(absPath.view())},
        {"env-SCRIPT_NAME", "/test.cgi"},
    };
    runCgiTest(*h, "GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

TEST(GoCGI, PathInfo) {
    auto h = newHandler("/test.cgi");
    strmap expectedMap = {
        {"param-a", "b"},
        {"env-PATH_INFO", "/extrapath"},
        {"env-QUERY_STRING", "a=b"},
        {"env-REQUEST_URI", "/test.cgi/extrapath?a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-SCRIPT_NAME", "/test.cgi"},
    };
    runCgiTest(*h, "GET /test.cgi/extrapath?a=b HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

TEST(GoCGI, PathInfoDirRoot) {
    auto h = newHandler("/myscript//");
    strmap expectedMap = {
        {"env-PATH_INFO", "/bar"},
        {"env-QUERY_STRING", "a=b"},
        {"env-REQUEST_URI", "/myscript/bar?a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-SCRIPT_NAME", "/myscript"},
    };
    runCgiTest(*h, "GET /myscript/bar?a=b HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

TEST(GoCGI, DupHeaders) {
    auto h = newHandler();
    strmap expectedMap = {
        {"env-REQUEST_URI", "/myscript/bar?a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-HTTP_COOKIE", "nom=NOM; yum=YUM"},
        {"env-HTTP_X_FOO", "val1, val2"},
    };
    runCgiTest(*h,
               "GET /myscript/bar?a=b HTTP/1.0\n"
               "Cookie: nom=NOM\n"
               "Cookie: yum=YUM\n"
               "X-Foo: val1\n"
               "X-Foo: val2\n"
               "Host: example.com\n\n",
               expectedMap);
}

// Issue 16405: CGI+http.Transport differing uses of HTTP_PROXY.
// Verify we don't set the HTTP_PROXY environment variable.
// Hope nobody was depending on it. It's not a known header, though.
TEST(GoCGI, DropProxyHeader) {
    auto h = newHandler();
    strmap expectedMap = {
        {"env-REQUEST_URI", "/myscript/bar?a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-HTTP_X_FOO", "a"},
    };
    runCgiTest(
        *h,
        "GET /myscript/bar?a=b HTTP/1.0\n"
        "X-Foo: a\n"
        "Proxy: should_be_stripped\n"
        "Host: example.com\n\n",
        expectedMap, {[](const strmap& reqInfo) {
            auto it = reqInfo.find("env-HTTP_PROXY");
            EXPECT_TRUE(it == reqInfo.end()) << "HTTP_PROXY = " << it->second << "; should be absent";
        }});
}

TEST(GoCGI, PathInfoNoRoot) {
    auto h = newHandler("");
    strmap expectedMap = {
        {"env-PATH_INFO", "/bar"},
        {"env-QUERY_STRING", "a=b"},
        {"env-REQUEST_URI", "/bar?a=b"},
        {"env-SCRIPT_FILENAME", exe()},
        {"env-SCRIPT_NAME", ""},
    };
    runCgiTest(*h, "GET /bar?a=b HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

TEST(GoCGI, CGIBasicPost) {
    std::string postReq =
        "POST /test.cgi?a=b HTTP/1.0\n"
        "Host: example.com\n"
        "Content-Type: application/x-www-form-urlencoded\n"
        "Content-Length: 15\n"
        "\n"
        "postfoo=postbar";
    auto h = newHandler("/test.cgi");
    strmap expectedMap = {
        {"test", "Hello CGI"},
        {"param-postfoo", "postbar"},
        {"env-REQUEST_METHOD", "POST"},
        {"env-CONTENT_LENGTH", "15"},
        {"env-REQUEST_URI", "/test.cgi?a=b"},
    };
    runCgiTest(*h, postReq, expectedMap);
}

// The CGI spec doesn't allow chunked requests.
TEST(GoCGI, CGIPostChunked) {
    std::string postReq =
        "POST /test.cgi?a=b HTTP/1.1\n"
        "Host: example.com\n"
        "Content-Type: application/x-www-form-urlencoded\n"
        "Transfer-Encoding: chunked\n"
        "\n" +
        chunk("postfoo") + chunk("=") + chunk("postbar") + chunk("");
    auto h = newHandler("/test.cgi");
    auto resp = runCgiTest(*h, postReq, {});
    EXPECT_EQ(resp->Code, http::StatusBadRequest) << "Expected response code from chunked request body";
}

// A chunked body of a few MiB, which AllowChunked streams to the program.
TEST(GoCGI, CGIPostChunkedAllowed) {
    std::string data(3 << 20, '\0');
    std::uint64_t want = 14695981039346656037ull;
    for (std::size_t i = 0; i < data.size(); i++) {
        data[i] = char((i * 7 + i / 251) & 0xff);
        want = (want ^ std::uint8_t(data[i])) * 1099511628211ull;
    }
    auto h = newHandler("/test.cgi");
    h->AllowChunked = true;
    auto req = newRequest(
        "POST /test.cgi?sum=1 HTTP/1.1\n"
        "Host: example.com\n"
        "Content-Type: application/octet-stream\n"
        "Transfer-Encoding: chunked\n\n");
    req->Body = io::NopCloser(strings::NewReader(string(data)));
    auto rw = httptest::NewRecorder();
    h->ServeHTTP(*rw, *req);
    EXPECT_EQ(rw->Code, 200);
    runResponseChecks(*rw, {{"bytes", std::to_string(data.size())}, {"sum", std::to_string(want)}});
}

// A Location of a local path goes to the client unless there is a
// PathLocationHandler.
TEST(GoCGI, RedirectLocalPathWithoutHandler) {
    auto h = newHandler("/test.cgi");
    auto rec = runCgiTest(*h, "GET /test.cgi?loc=/foo HTTP/1.0\nHost: example.com\n\n", {});
    EXPECT_EQ(rec->Code, 302);
    EXPECT_EQ(rec->Header().Get("Location"), "/foo");
}

TEST(GoCGI, Redirect) {
    auto h = newHandler("/test.cgi");
    auto rec = runCgiTest(*h, "GET /test.cgi?loc=http://foo.com/ HTTP/1.0\nHost: example.com\n\n", {});
    EXPECT_EQ(rec->Code, 302);
    EXPECT_EQ(rec->Header().Get("Location"), "http://foo.com/");
}

TEST(GoCGI, InternalRedirect) {
    auto baseHandler = make_ptr<http::HandlerFunc>([](http::ResponseWriter& rw, http::Request& req) {
        io::WriteString(rw, std::format("basepath={}\n", req.URL.Path.view()));
        io::WriteString(rw, std::format("remoteaddr={}\n", req.RemoteAddr.view()));
    });
    auto h = newHandler("/test.cgi");
    h->PathLocationHandler = baseHandler;
    strmap expectedMap = {
        {"basepath", "/foo"},
        {"remoteaddr", "1.2.3.4:1234"},
    };
    runCgiTest(*h, "GET /test.cgi?loc=/foo HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

// After an internal redirect the program is not killed: it finishes its work.
TEST(GoCGI, InternalRedirectWaitsForChild) {
    auto [dir, derr] = os::MkdirTemp("", "cgi-marker");
    ASSERT_FALSE(derr) << derr;
    std::string marker = std::string(dir.view()) + "/done";
    auto base = make_ptr<http::HandlerFunc>([](http::ResponseWriter& rw, http::Request& req) {
        io::WriteString(rw, "ok\n");
    });
    auto h = newHandler("/test.cgi");
    h->PathLocationHandler = base;
    auto rw = httptest::NewRecorder();
    auto req = newRequest("GET /test.cgi?loc=/foo&marker=" + marker + " HTTP/1.0\nHost: example.com\n\n");
    h->ServeHTTP(*rw, *req);
    EXPECT_EQ(rw->Body, "ok\n");
    auto [fi, serr] = os::Stat(marker);
    EXPECT_FALSE(serr) << "the CGI program was killed before it finished: " << serr;
    os::RemoveAll(dir);
}

// TestCopyError tests that we kill the process if there's an error copying
// its output. (for example, from the client having gone away)
//
// If we fail to do so, the test will hang in a call to ServeHTTP that is
// blocked in Cmd::Wait.
TEST(GoCGI, CopyError) {
    auto h = newHandler("/test.cgi");
    std::atomic<int> running{0};
    server ts(make_ptr<http::HandlerFunc>([&](http::ResponseWriter& w, http::Request& r) {
        running++;
        h->ServeHTTP(w, r);
        running--;
    }));

    auto [conn, err] = net::Dial("tcp", ts->Listener->Addr()->String());
    ASSERT_FALSE(err) << err;
    std::string req = "GET /test.cgi?bigresponse=1 HTTP/1.0\r\nHost: example.com\r\n\r\n";
    auto [wn, werr] = conn->Write(std::span<const byte>(reinterpret_cast<const byte*>(req.data()), req.size()));
    ASSERT_FALSE(werr) << werr;
    // The headers and 5000 bytes of the body.
    std::string got;
    std::vector<byte> buf(4096);
    while (got.size() < 5000 + 200) {
        auto [n, rerr] = conn->Read(buf);
        got.append(reinterpret_cast<const char*>(buf.data()), std::size_t(n));
        ASSERT_FALSE(rerr) << "Read: " << got.size() << " bytes, " << rerr;
    }
    ASSERT_NE(got.find("\r\n\r\n"), std::string::npos);

    EXPECT_NE(running.load(), 0) << "pre-conn.Close, expected handler to still be running";
    conn->Close();
    auto closed = time::Now();

    auto nextSleep = 1 * time::Millisecond;
    for (int i = 0; running.load() != 0; i++) {
        ASSERT_LT(i, 14) << "handler still running " << time::Since(closed).String().view() << " after conn.Close";
        time::Sleep(nextSleep);
        nextSleep = nextSleep * 2;
    }
}

TEST(GoCGI, Dir) {
    auto [cwd, err] = os::Getwd();
    ASSERT_FALSE(err);
    auto h = newHandler("/test.cgi");
    h->Dir = cwd;
    runCgiTest(*h, "GET /test.cgi HTTP/1.0\nHost: example.com\n\n", {{"cwd", std::string(cwd.view())}});

    // No Dir: the directory of Path.
    auto [dir, file] = filepath::Split(exe());
    h = newHandler("/test.cgi");
    runCgiTest(*h, "GET /test.cgi HTTP/1.0\nHost: example.com\n\n", {{"cwd", std::string(dir.view())}});
}

TEST(GoCGI, EnvOverride) {
    auto [cgifile, e1] = filepath::Abs("testdata/test.cgi");
    auto [cwd, e2] = os::Getwd();
    auto h = newHandler("/test.cgi", {string("SCRIPT_FILENAME=" + std::string(cgifile.view())),
                                       "REQUEST_URI=/foo/bar", "PATH=/wibble"});
    h->Dir = cwd;
    strmap expectedMap = {
        {"cwd", std::string(cwd.view())},
        {"env-SCRIPT_FILENAME", std::string(cgifile.view())},
        {"env-REQUEST_URI", "/foo/bar"},
        {"env-PATH", "/wibble"},
    };
    runCgiTest(*h, "GET /test.cgi HTTP/1.0\nHost: example.com\n\n", expectedMap);
}

TEST(GoCGI, HandlerStderr) {
    auto stderr_ = make_ptr<strings::Builder>();
    auto h = newHandler("/test.cgi");
    h->Stderr = stderr_;

    auto rw = httptest::NewRecorder();
    auto req = newRequest("GET /test.cgi?writestderr=1 HTTP/1.0\nHost: example.com\n\n");
    h->ServeHTTP(*rw, *req);
    EXPECT_EQ(std::string(stderr_->String().view()), "Hello, stderr!\n");
}

TEST(GoCGI, RemoveLeadingDuplicates) {
    struct {
        slice<string> env, want;
    } tests[] = {
        {{"a=b", "b=c", "a=b2"}, {"b=c", "a=b2"}},
        {{"a=b", "b=c", "d", "e=f"}, {"a=b", "b=c", "d", "e=f"}},
    };
    for (auto& tt : tests) {
        auto got = cgi::detail::removeLeadingDuplicates(tt.env);
        EXPECT_EQ(std::vector<string>(got.begin(), got.end()), std::vector<string>(tt.want.begin(), tt.want.end()));
    }
}

// The response passes through in 64 KiB writes, 256 MiB in all, without
// the handler or the server holding it.
TEST(GoCGI, CGIStreamsLargeBody) {
    constexpr int64 total = 268435456;
    auto h = newHandler("/test.cgi");
    server ts(h);

    struct rusage before {}, after {};
    ::getrusage(RUSAGE_SELF, &before);
    auto [req, e0] = http::NewRequest("GET", std::string(ts->URL.view()) + "/test.cgi?stream=" + std::to_string(total));
    ASSERT_FALSE(e0);
    auto [res, err] = ts->Client().Do(req);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(res->StatusCode, 200);
    int64 n = 0;
    std::vector<byte> buf(64 << 10);
    for (;;) {
        auto [r, rerr] = res->Body->Read(buf);
        n += r;
        if (rerr) {
            EXPECT_EQ(rerr, io::EOF_) << rerr;
            break;
        }
    }
    res->Body->Close();
    ::getrusage(RUSAGE_SELF, &after);
    EXPECT_EQ(n, total);
    long growthKB = after.ru_maxrss - before.ru_maxrss;  // ru_maxrss is in KiB
    EXPECT_LT(growthKB, 64 * 1024) << "peak RSS grew by " << growthKB << " KiB";
}

// The client reads 1 MiB of an endless response and goes away: the handler
// kills the CGI program and reaps it.
TEST(GoCGI, CGIClientDisconnectKillsChild) {
    auto h = newHandler("/test.cgi");
    server ts(h);

    auto [req, e0] = http::NewRequest("GET", std::string(ts->URL.view()) + "/test.cgi?forever=1");
    ASSERT_FALSE(e0);
    auto [res, err] = ts->Client().Do(req);
    ASSERT_FALSE(err) << err;
    int pid = std::atoi(std::string(res->Header.Get("X-CGI-Pid").view()).c_str());
    ASSERT_GT(pid, 0);

    int64 n = 0;
    std::vector<byte> buf(64 << 10);
    while (n < (1 << 20)) {
        auto [r, rerr] = res->Body->Read(buf);
        n += r;
        ASSERT_FALSE(rerr) << rerr;
    }
    res->Body->Close();

    auto start = time::Now();
    bool gone = false;
    while (time::Since(start) < 5 * time::Second) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            gone = true;
            break;
        }
        time::Sleep(10 * time::Millisecond);
    }
    EXPECT_TRUE(gone) << "the CGI program " << pid << " is still there 5 s after the client left";
    if (!gone) ::kill(pid, SIGKILL);
}

}  // namespace
