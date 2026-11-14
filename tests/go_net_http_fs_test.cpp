// Tests ported from Go 1.27.3's net/http/fs_test.go: FileServer, ServeFile
// and ServeContent, over HTTP/1.1, HTTPS and HTTP/2 where Go runs them in
// those modes; TestLinuxSendfile runs the server as a helper command (see
// go_helper.hpp) under strace. Left out:
// TestServeFileZippingResponseWriter (no compress/gzip), the
// GODEBUG=httpservecontentkeepheaders=1 variants (no GODEBUG), and the
// case of TestServeFileContentType that sets Content-Type to no values (a
// go++ Header can't hold a key without one).
//
// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in Go's LICENSE file.
#include "go_helper.hpp"
#include "go_os_test_util.hpp"
#include "net/http/internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/mime.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/path.hpp>
#include <gopp/strings.hpp>
#include <gopp/testing/fstest.hpp>

#include <gtest/gtest.h>

#include <ctime>
#include <map>
#include <optional>

using namespace gopp;
namespace exec = gopp::os::exec;
using os_test::Chdir;

// Go's test modes: HTTP/1.1, HTTP/1.1 over TLS, and HTTP/2 over TLS. (Not
// in the anonymous namespace: googletest's parameterized tests hold one.)
enum class httpFsTestMode { http1, https1, http2 };

namespace {

using testMode = httpFsTestMode;

std::string modeName(const testing::TestParamInfo<testMode>& info) {
    switch (info.param) {
    case testMode::http1: return "h1";
    case testMode::https1: return "https1";
    case testMode::http2: return "h2";
    }
    return "";
}

constexpr std::string_view testFile = "testdata/file";
constexpr int64 testFileLen = 11;

std::string str(const string& s) { return std::string(s.view()); }
std::string str(std::span<const byte> b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

// time.Date in UTC.
time::Time date(int y, int mon, int d, int h, int min, int s, int64 ns) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = min;
    tm.tm_sec = s;
    return time::Time::Unix(timegm(&tm), ns);
}

// t.TempDir(). (os_test::TempDir names it after the test, and the names
// of parameterized tests have slashes.)
class TempDir {
public:
    TempDir() {
        auto [d, err] = os::MkdirTemp("", "gopp_http_fs");
        if (err) ADD_FAILURE() << "MkdirTemp: " << err;
        path_ = str(d);
    }
    ~TempDir() {
        if (gopp::error err = os::RemoveAll(path_)) ADD_FAILURE() << "TempDir RemoveAll cleanup: " << err;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const noexcept { return path_; }
    operator std::string_view() const noexcept { return path_; }

private:
    std::string path_;
};

// Go's testdata directory, made in a temporary directory that is the
// working directory during the test, with fs_test.go for the tests that
// serve a file of the working directory.
class GoNetHttpFS : public testing::Test {
protected:
    void SetUp() override {
        write("testdata/file", "0123456789\n");
        write("testdata/index.html", "index.html says hello\n");
        write("testdata/style.css", "body {}\n");
        write("fs_test.go", "package http_test\n");
        cd_.emplace(dir_.path());
    }
    void TearDown() override { cd_.reset(); }

    void write(const std::string& name, std::string_view data) {
        std::string p = os_test::join(dir_, name);
        ASSERT_FALSE(os::MkdirAll(path::Dir(p), 0755));
        ASSERT_FALSE(os::WriteFile(p, data, 0644));
    }

    TempDir dir_;
    std::optional<Chdir> cd_;
};


class GoNetHttpFSModes : public GoNetHttpFS, public testing::WithParamInterface<testMode> {
protected:
    // newClientServerTest: a test server in the mode, closed at the end.
    httptest::Server& serve(ptr<http::Handler> h) {
        ptr<httptest::Server> ts;
        http::Protocols p;
        switch (GetParam()) {
        case testMode::http1:
            ts = httptest::NewServer(std::move(h));
            break;
        case testMode::https1:
            p.SetHTTP1(true);
            ts = httptest::NewTLSServer(std::move(h), p);
            break;
        case testMode::http2:
            p.SetHTTP2(true);
            ts = httptest::NewTLSServer(std::move(h), p);
            break;
        }
        servers_.push_back(ts);
        return *ts;
    }
    httptest::Server& serve(http::handlerFn f) { return serve(make_ptr<http::HandlerFunc>(std::move(f))); }

    void TearDown() override {
        for (auto& ts : servers_) ts->Close();
        servers_.clear();
        GoNetHttpFS::TearDown();
    }

    std::vector<ptr<httptest::Server>> servers_;
};

INSTANTIATE_TEST_SUITE_P(Modes, GoNetHttpFSModes,
                         testing::Values(testMode::http1, testMode::https1, testMode::http2), modeName);

// The body of a response, read to the end and closed.
std::string readBody(const ptr<http::Response>& res) {
    auto [b, err] = io::ReadAll(*res->Body);
    EXPECT_FALSE(err) << "reading Body: " << err;
    res->Body->Close();
    return str(b);
}

struct getResult {
    ptr<http::Response> resp;
    std::string body;
};

getResult get(http::Client& c, std::string_view url) {
    auto [res, err] = c.Get(url);
    if (err) {
        ADD_FAILURE() << "Get " << url << ": " << err;
        return {};
    }
    return {res, readBody(res)};
}

getResult doRequest(http::Client& c, const ptr<http::Request>& req) {
    auto [res, err] = c.Do(req);
    if (err) {
        ADD_FAILURE() << req->Method << " " << req->URL.String() << ": " << err;
        return {};
    }
    return {res, readBody(res)};
}

ptr<http::Request> newRequest(std::string_view method, std::string_view url) {
    auto [req, err] = http::NewRequest(method, url);
    EXPECT_FALSE(err) << err;
    return req;
}

std::string readFile(std::string_view name) {
    auto [b, err] = os::ReadFile(name);
    EXPECT_FALSE(err) << "reading file: " << err;
    return str(b);
}

// A part of a multipart body (mime/multipart's Reader, for the tests).
struct part {
    std::map<std::string, std::string> header;
    std::string body;
};

// The parts of a multipart body with the boundary; nothing if it's
// malformed.
std::optional<std::vector<part>> readMultipart(std::string_view body, std::string_view boundary) {
    std::string delim = "--" + std::string(boundary);
    if (!body.starts_with(delim)) return std::nullopt;
    std::vector<part> parts;
    std::size_t pos = delim.size();
    for (;;) {
        if (body.substr(pos, 2) == "--") break;  // the end
        if (body.substr(pos, 2) != "\r\n") return std::nullopt;
        pos += 2;
        part p;
        for (;;) {
            auto eol = body.find("\r\n", pos);
            if (eol == body.npos) return std::nullopt;
            std::string_view line = body.substr(pos, eol - pos);
            pos = eol + 2;
            if (line.empty()) break;
            auto colon = line.find(": ");
            if (colon == line.npos) return std::nullopt;
            p.header[std::string(line.substr(0, colon))] = std::string(line.substr(colon + 2));
        }
        auto next = body.find("\r\n" + delim, pos);
        if (next == body.npos) return std::nullopt;
        p.body = std::string(body.substr(pos, next - pos));
        parts.push_back(std::move(p));
        pos = next + 2 + delim.size();
    }
    return parts;
}

// ---------------------------------------------------------------------------
// A fake file system (fakeFS)

struct fakeFileInfo final : public fs::FileInfo {
    bool dir = false;
    std::string basename;
    time::Time modtime;
    std::vector<ptr<fakeFileInfo>> ents;
    std::string contents;
    gopp::error err;

    string Name() const override { return string(basename); }
    int64 Size() const override { return static_cast<int64>(contents.size()); }
    fs::FileMode Mode() const override { return dir ? fs::ModeDir | 0755 : fs::FileMode(0644); }
    time::Time ModTime() const override { return modtime; }
    bool IsDir() const override { return dir; }
};

class fakeFile final : public http::File {
public:
    fakeFile(ptr<fakeFileInfo> fi, std::string path)
        : rs_(strings::NewReader(string(fi->contents))), fi_(std::move(fi)), path_(std::move(path)) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override { return rs_->Read(p); }
    std::pair<int64, gopp::error> Seek(int64 offset, int whence) override { return rs_->Seek(offset, whence); }
    gopp::error Close() override { return nullptr; }
    std::pair<ptr<fs::FileInfo>, gopp::error> Stat() override { return {fi_, nullptr}; }

    std::pair<std::vector<ptr<fs::FileInfo>>, gopp::error> Readdir(int count) override {
        if (!fi_->dir) return {{}, fs::ErrInvalid};
        std::vector<ptr<fs::FileInfo>> fis;
        std::size_t limit = entpos_ + static_cast<std::size_t>(count > 0 ? count : 0);
        if (count <= 0 || limit > fi_->ents.size()) limit = fi_->ents.size();
        for (; entpos_ < limit; entpos_++) fis.push_back(fi_->ents[entpos_]);
        if (fis.empty() && count > 0) return {fis, io::EOF_};
        return {fis, nullptr};
    }

private:
    ptr<io::StringReader> rs_;
    ptr<fakeFileInfo> fi_;
    std::string path_;  // as opened
    std::size_t entpos_ = 0;
};

class fakeFs final : public http::FileSystem {
public:
    std::map<std::string, ptr<fakeFileInfo>> files;

    std::pair<ptr<http::File>, gopp::error> Open(std::string_view name) override {
        std::string n = str(path::Clean(name));
        auto it = files.find(n);
        if (it == files.end()) return {nullptr, fs::ErrNotExist};
        if (it->second->err) return {nullptr, it->second->err};
        return {make_ptr<fakeFile>(it->second, n), nullptr};
    }
};

// testFileSystem: Open is a function.
class testFileSystem final : public http::FileSystem {
public:
    explicit testFileSystem(std::function<std::pair<ptr<http::File>, gopp::error>(std::string_view)> open)
        : open_(std::move(open)) {}
    std::pair<ptr<http::File>, gopp::error> Open(std::string_view name) override { return open_(name); }

private:
    std::function<std::pair<ptr<http::File>, gopp::error>(std::string_view)> open_;
};

ptr<fakeFileInfo> fakeInfo(std::string basename, time::Time modtime = {}, std::string contents = {},
                              bool dir = false, std::vector<ptr<fakeFileInfo>> ents = {}) {
    auto fi = make_ptr<fakeFileInfo>();
    fi->basename = std::move(basename);
    fi->modtime = modtime;
    fi->contents = std::move(contents);
    fi->dir = dir;
    fi->ents = std::move(ents);
    return fi;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST_P(GoNetHttpFSModes, ServeFile) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) { http::ServeFile(w, r, "testdata/file"); });
    auto& c = ts.Client();
    std::string file = readFile(testFile);

    // Get contents via various methods. See https://go.dev/issue/59471 for
    // a proposal to limit the set of methods handled. For now, test the
    // historical behavior.
    for (std::string_view method : {http::MethodGet, http::MethodPost, http::MethodPut, http::MethodPatch,
                                    http::MethodDelete, http::MethodOptions, http::MethodTrace}) {
        auto [resp, body] = doRequest(c, newRequest(method, ts.URL));
        ASSERT_EQ(body, file) << "body mismatch for " << method << " request";
    }

    // HEAD request.
    {
        auto [resp, body] = doRequest(c, newRequest(http::MethodHead, ts.URL));
        ASSERT_EQ(body, "") << "body mismatch for HEAD request";
        ASSERT_EQ(resp->Header.Get("Content-Length"), fmt::Sprint("{}", file.size()));
    }

    // Range tests.
    struct wantRange {
        int64 start, end;  // range [start,end)
    };
    struct {
        std::string r;
        int code;
        std::vector<wantRange> ranges;
    } tests[] = {
        {"", http::StatusOK, {}},
        {"bytes=0-4", http::StatusPartialContent, {{0, 5}}},
        {"bytes=2-", http::StatusPartialContent, {{2, testFileLen}}},
        {"bytes=-5", http::StatusPartialContent, {{testFileLen - 5, testFileLen}}},
        {"bytes=3-7", http::StatusPartialContent, {{3, 8}}},
        {"bytes=0-0,-2", http::StatusPartialContent, {{0, 1}, {testFileLen - 2, testFileLen}}},
        {"bytes=0-1,5-8", http::StatusPartialContent, {{0, 2}, {5, 9}}},
        {"bytes=0-1,5-", http::StatusPartialContent, {{0, 2}, {5, testFileLen}}},
        {"bytes=5-1000", http::StatusPartialContent, {{5, testFileLen}}},
        {"bytes=0-,1-,2-,3-,4-", http::StatusOK, {}},  // ignore wasteful range request
        {"bytes=0-9", http::StatusPartialContent, {{0, testFileLen - 1}}},
        {"bytes=0-10", http::StatusPartialContent, {{0, testFileLen}}},
        {"bytes=0-11", http::StatusPartialContent, {{0, testFileLen}}},
        {"bytes=10-11", http::StatusPartialContent, {{testFileLen - 1, testFileLen}}},
        {"bytes=10-", http::StatusPartialContent, {{testFileLen - 1, testFileLen}}},
        {"bytes=11-", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=11-12", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=12-12", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=11-100", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=12-100", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=100-", http::StatusRequestedRangeNotSatisfiable, {}},
        {"bytes=100-1000", http::StatusRequestedRangeNotSatisfiable, {}},
    };
    for (const auto& rt : tests) {
        auto req = newRequest(http::MethodGet, ts.URL);
        if (!rt.r.empty()) req->Header.Set("Range", string(rt.r));
        auto [resp, body] = doRequest(c, req);
        EXPECT_EQ(resp->StatusCode, rt.code) << "range=" << rt.r;
        if (rt.code == http::StatusRequestedRangeNotSatisfiable) continue;
        std::string wantContentRange;
        if (rt.ranges.size() == 1) {
            wantContentRange = fmt::Sprint("bytes {}-{}/{}", rt.ranges[0].start, rt.ranges[0].end - 1, testFileLen).str();
        }
        EXPECT_EQ(resp->Header.Get("Content-Range"), wantContentRange) << "range=" << rt.r;
        string ct = resp->Header.Get("Content-Type");
        if (rt.ranges.size() == 1) {
            const auto& rng = rt.ranges[0];
            EXPECT_EQ(body, file.substr(rng.start, rng.end - rng.start)) << "range=" << rt.r;
            EXPECT_FALSE(ct.starts_with("multipart/byteranges")) << "range=" << rt.r << " content-type = " << ct;
        }
        if (rt.ranges.size() > 1) {
            auto [typ, params, err] = mime::ParseMediaType(ct);
            if (err) {
                ADD_FAILURE() << "range=" << rt.r << " content-type = " << ct << "; " << err;
                continue;
            }
            EXPECT_EQ(typ, "multipart/byteranges") << "range=" << rt.r;
            string boundary = params[string::fromStatic("boundary")];
            if (boundary.empty()) {
                ADD_FAILURE() << "range=" << rt.r << " content-type = " << ct << "; lacks boundary";
                continue;
            }
            EXPECT_EQ(resp->ContentLength, static_cast<int64>(body.size())) << "range=" << rt.r;
            auto parts = readMultipart(body, boundary);
            if (!parts) {
                ADD_FAILURE() << "range=" << rt.r << ": malformed multipart body " << testing::PrintToString(body);
                continue;
            }
            ASSERT_EQ(parts->size(), rt.ranges.size()) << "range=" << rt.r;
            for (std::size_t ri = 0; ri < rt.ranges.size(); ri++) {
                const auto& rng = rt.ranges[ri];
                EXPECT_EQ((*parts)[ri].header["Content-Range"],
                          fmt::Sprint("bytes {}-{}/{}", rng.start, rng.end - 1, testFileLen).str())
                    << "range=" << rt.r << ": part " << ri;
                EXPECT_EQ((*parts)[ri].body, file.substr(rng.start, rng.end - rng.start))
                    << "range=" << rt.r << ": part " << ri;
            }
        }
    }
}

TEST_F(GoNetHttpFS, ServeFile_DotDot) {
    struct {
        std::string req;
        int wantStatus;
    } tests[] = {
        {"/testdata/file", 200}, {"/../file", 400},  {"/..", 400},         {"/../", 400},
        {"/../foo", 400},        {"/..\\foo", 400},  {"/file/a", 200},     {"/file/a..", 200},
        {"/file/a/..", 400},     {"/file/a\\..", 400},
    };
    for (const auto& tt : tests) {
        auto br = make_ptr<bufio::Reader>(strings::NewReader(string("GET " + tt.req + " HTTP/1.1\r\nHost: foo\r\n\r\n")));
        auto [req, err] = http::internal::readRequest(br, http::DefaultMaxHeaderBytes);
        if (err) {
            ADD_FAILURE() << "bad request " << tt.req << ": " << err;
            continue;
        }
        auto rec = httptest::NewRecorder();
        http::ServeFile(*rec, *req, "testdata/file");
        EXPECT_EQ(rec->Code, tt.wantStatus) << "for request " << tt.req;
    }
}

// Tests that this doesn't panic. (Issue 30165)
TEST_F(GoNetHttpFS, ServeFileDirPanicEmptyPath) {
    auto rec = httptest::NewRecorder();
    auto req = httptest::NewRequest("GET", "/");
    req->URL.Path = {};
    http::ServeFile(*rec, *req, "testdata");
    EXPECT_EQ(rec->Result()->StatusCode, 301);
}

// Tests that ranges are ignored with serving empty content. (Issue 54794)
TEST_F(GoNetHttpFS, ServeContentWithEmptyContentIgnoreRanges) {
    for (std::string_view r : {"bytes=0-128", "bytes=1-"}) {
        auto rec = httptest::NewRecorder();
        auto req = httptest::NewRequest("GET", "/");
        req->Header.Set("Range", string(r));
        auto content = strings::NewReader("");
        http::ServeContent(*rec, *req, "nothing", time::Now(), *content);
        EXPECT_EQ(rec->Result()->StatusCode, 200) << r;
        EXPECT_EQ(rec->Body.size(), 0u) << r;
    }
}

TEST_P(GoNetHttpFSModes, FSRedirect) {
    auto& ts = serve(http::StripPrefix(string::fromStatic("/test"), http::FileServer(http::Dir("."))));
    struct {
        std::string original, redirect;
        int status;
    } tests[] = {
        {"/test/index.html", "/test/", 200},
        {"/test/testdata", "/test/testdata/", 200},
        {"/test/testdata/file/", "/test/testdata/file", 200},
        // Redirect attempts for path with escaped slashes should result in 404.
        // However, escaped paths are okay if they do not trigger a redirect.
        // See https://go.dev/issue/80289.
        {"/test%2ftestdata", "/test/testdata", 404},
        {"/test/testdata%2ffile/", "/test/testdata/file/", 404},
        {"/test/testdata%2Findex.html", "/test/testdata/index.html", 404},
        {"/test/testdata%2ffile", "/test/testdata/file", 200},
        {"/test/testdata%2F", "/test/testdata/", 200},
    };
    for (const auto& data : tests) {
        auto [res, body] = get(ts.Client(), str(ts.URL + std::string_view(data.original)));
        ASSERT_TRUE(res);
        EXPECT_EQ(res->Request->URL.Path, data.redirect) << "redirect from " << data.original;
        EXPECT_EQ(res->StatusCode, data.status) << "redirect from " << data.original;
    }
}

TEST_F(GoNetHttpFS, FileServerCleans) {
    std::vector<std::string> opened;
    auto fsys = http::FileServer(make_ptr<testFileSystem>([&](std::string_view name) {
        opened.emplace_back(name);
        return std::pair<ptr<http::File>, gopp::error>(nullptr, errors::New("file does not exist"));
    }));
    struct {
        std::string reqPath, openArg;
    } tests[] = {
        {"/foo.txt", "/foo.txt"},
        {"//foo.txt", "/foo.txt"},
        {"/../foo.txt", "/foo.txt"},
    };
    auto req = newRequest("GET", "http://example.com");
    for (const auto& test : tests) {
        auto rec = httptest::NewRecorder();
        req->URL.Path = string(test.reqPath);
        opened.clear();
        fsys->ServeHTTP(*rec, *req);
        ASSERT_EQ(opened.size(), 1u) << test.reqPath;
        EXPECT_EQ(opened[0], test.openArg) << test.reqPath;
    }
}

TEST_P(GoNetHttpFSModes, FileServerEscapesNames) {
    constexpr std::string_view dirListPrefix =
        "<!doctype html>\n<meta name=\"viewport\" content=\"width=device-width\">\n<pre>\n";
    constexpr std::string_view dirListSuffix = "\n</pre>\n";
    struct {
        std::string name, escaped;
    } tests[] = {
        {R"(simple_name)", R"(<a href="simple_name">simple_name</a>)"},
        {R"("'<>&)", R"(<a href="%22%27%3C%3E&">&#34;&#39;&lt;&gt;&amp;</a>)"},
        {R"(?foo=bar#baz)", R"(<a href="%3Ffoo=bar%23baz">?foo=bar#baz</a>)"},
        {R"(<combo>?foo)", R"(<a href="%3Ccombo%3E%3Ffoo">&lt;combo&gt;?foo</a>)"},
        {R"(foo:bar)", R"(<a href="./foo:bar">foo:bar</a>)"},
    };

    // We put each test file in its own directory in the fakeFS so we can
    // look at it in isolation.
    auto fsys = make_ptr<fakeFs>();
    for (std::size_t i = 0; i < std::size(tests); i++) {
        auto testFile = fakeInfo(tests[i].name);
        fsys->files[fmt::Sprint("/{}", i).str()] = fakeInfo("", time::Time::Unix(1000000000, 0), "", true, {testFile});
        fsys->files[fmt::Sprint("/{}/{}", i, tests[i].name).str()] = testFile;
    }

    auto& ts = serve(http::FileServer(fsys));
    for (std::size_t i = 0; i < std::size(tests); i++) {
        auto [res, s] = get(ts.Client(), fmt::Sprint("{}/{}", ts.URL, i).view());
        ASSERT_TRUE(res);
        if (!s.starts_with(dirListPrefix) || !s.ends_with(dirListSuffix)) {
            ADD_FAILURE() << "test " << tests[i].name << ": listing dir, full output is " << testing::PrintToString(s);
            continue;
        }
        std::string trimmed = s.substr(dirListPrefix.size(), s.size() - dirListPrefix.size() - dirListSuffix.size());
        EXPECT_EQ(trimmed, tests[i].escaped) << "test " << tests[i].name << ": listing dir, filename escaped";
    }
}

TEST_P(GoNetHttpFSModes, FileServerSortsNames) {
    constexpr std::string_view contents = "I am a fake file";
    auto dirMod = time::Time::Unix(123, 0);
    auto fileMod = time::Time::Unix(1000000000, 0);
    auto fsys = make_ptr<fakeFs>();
    fsys->files["/"] = fakeInfo("", dirMod, "", true,
                                 {fakeInfo("b", fileMod, std::string(contents)),
                                  fakeInfo("a", fileMod, std::string(contents))});

    auto& ts = serve(http::FileServer(fsys));
    auto [res, s] = get(ts.Client(), ts.URL);
    EXPECT_NE(s.find("<a href=\"a\">a</a>\n<a href=\"b\">b</a>"), s.npos) << "output appears to be unsorted:\n" << s;
}

TEST_P(GoNetHttpFSModes, FileServerImplicitLeadingSlash) {
    TempDir tempDir;
    ASSERT_FALSE(os::WriteFile(os_test::join(tempDir, "foo.txt"), "Hello world", 0644));
    auto& ts = serve(http::StripPrefix(string::fromStatic("/bar/"), http::FileServer(http::Dir(string(tempDir.path())))));
    auto getBody = [&](std::string_view suffix) { return get(ts.Client(), str(ts.URL + suffix)).body; };
    std::string s = getBody("/bar/");
    EXPECT_NE(s.find(">foo.txt<"), s.npos) << "expected a directory listing with foo.txt, got " << s;
    EXPECT_EQ(getBody("/bar/foo.txt"), "Hello world");
}

TEST_F(GoNetHttpFS, DirJoin) {
    auto [wfi, err] = os::Stat("/etc/hosts");
    if (err) GTEST_SKIP() << "skipping test; no /etc/hosts file";
    auto test = [&](const http::Dir& d, std::string_view name) {
        auto [f, oerr] = const_cast<http::Dir&>(d).Open(name);
        ASSERT_FALSE(oerr) << "open of " << name << ": " << oerr;
        auto [gfi, serr] = f->Stat();
        f->Close();
        ASSERT_FALSE(serr) << "stat of " << name << ": " << serr;
        EXPECT_TRUE(os::SameFile(gfi, wfi)) << name << " got different file";
    };
    test(http::Dir("/etc/"), "/hosts");
    test(http::Dir("/etc/"), "hosts");
    test(http::Dir("/etc/"), "../../../../hosts");
    test(http::Dir("/etc"), "/hosts");
    test(http::Dir("/etc"), "hosts");
    test(http::Dir("/etc"), "../../../../hosts");

    // Not really directories, but since we use this trick in ServeFile,
    // test it:
    test(http::Dir("/etc/hosts"), "");
    test(http::Dir("/etc/hosts"), "/");
    test(http::Dir("/etc/hosts"), "../");
}

TEST_F(GoNetHttpFS, EmptyDirOpenCWD) {
    auto test = [](http::Dir d) {
        auto [f, err] = d.Open("fs_test.go");
        ASSERT_FALSE(err) << "open of fs_test.go: " << err;
        f->Close();
    };
    test(http::Dir(""));
    test(http::Dir("."));
    test(http::Dir("./"));
}

TEST_P(GoNetHttpFSModes, ServeFileContentType) {
    constexpr std::string_view ctype = "icecream/chocolate";
    auto& ts = serve([&](http::ResponseWriter& w, http::Request& r) {
        if (r.FormValue("override") == "1") w.Header().Set("Content-Type", string(ctype));
        http::ServeFile(w, r, "testdata/file");
    });
    auto check = [&](std::string_view override, std::vector<std::string> want) {
        auto [res, body] = get(ts.Client(), str(ts.URL + "?override=" + override));
        ASSERT_TRUE(res);
        std::vector<std::string> h;
        for (const auto& v : res->Header.Values("Content-Type")) h.push_back(str(v));
        EXPECT_EQ(h, want) << "override=" << override;
    };
    check("0", {"text/plain; charset=utf-8"});
    check("1", {std::string(ctype)});
}

TEST_P(GoNetHttpFSModes, ServeFileMimeType) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) { http::ServeFile(w, r, "testdata/style.css"); });
    auto [res, body] = get(ts.Client(), ts.URL);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->Header.Get("Content-Type"), "text/css; charset=utf-8");
}

TEST_P(GoNetHttpFSModes, ServeFileFromCWD) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) { http::ServeFile(w, r, "fs_test.go"); });
    auto [res, body] = get(ts.Client(), ts.URL);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->StatusCode, 200) << res->Status;
}

// Issue 13996
TEST_P(GoNetHttpFSModes, ServeDirWithoutTrailingSlash) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) { http::ServeFile(w, r, "."); });
    auto [res, body] = get(ts.Client(), str(ts.URL + "/testdata"));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->Request->URL.Path, "/testdata/");
}

// Tests that ServeFile doesn't add a Content-Length if a Content-Encoding
// is specified.
TEST_P(GoNetHttpFSModes, ServeFileWithContentEncoding) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Encoding", string::fromStatic("foo"));
        http::ServeFile(w, r, "testdata/file");
        // The testdata is so small that the server would buffer all of it
        // and add its own Content-Length. To test ServeFile only, flush.
        dynamic_cast<http::Flusher&>(w).Flush();
    });
    auto [res, body] = get(ts.Client(), ts.URL);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->ContentLength, -1);
}

// Tests that ServeFile does not generate representation metadata when the
// file has not been modified, as per RFC 7232 section 4.1.
TEST_P(GoNetHttpFSModes, ServeFileNotModified) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", string::fromStatic("application/json"));
        w.Header().Set("Content-Encoding", string::fromStatic("foo"));
        w.Header().Set("Etag", string::fromStatic(R"("123")"));
        http::ServeFile(w, r, "testdata/file");
        dynamic_cast<http::Flusher&>(w).Flush();
    });
    auto req = newRequest("GET", ts.URL);
    req->Header.Set("If-None-Match", string::fromStatic(R"("123")"));
    auto [res, body] = doRequest(ts.Client(), req);
    ASSERT_TRUE(res);
    EXPECT_EQ(body, "") << "non-empty body";
    EXPECT_EQ(res->StatusCode, http::StatusNotModified);
    // The HTTP/1 transport sets ContentLength to 0.
    EXPECT_TRUE(res->ContentLength == -1 || res->ContentLength == 0) << res->ContentLength;
    EXPECT_EQ(res->Header.Get("Content-Type"), "") << "Content-Type present, but it should not be";
    EXPECT_EQ(res->Header.Get("Content-Encoding"), "") << "Content-Encoding present, but it should not be";
}

TEST_P(GoNetHttpFSModes, ServeIndexHtml) {
    for (int i = 0; i < 2; i++) {
        ptr<http::Handler> h = i == 0 ? http::FileServer(http::Dir(".")) : http::FileServer(http::FS(os::DirFS(".")));
        auto& ts = serve(h);
        for (std::string_view path : {"/testdata/", "/testdata/index.html"}) {
            auto [res, body] = get(ts.Client(), str(ts.URL + path));
            EXPECT_EQ(body, "index.html says hello\n") << (i == 0 ? "Dir" : "DirFS") << ": for path " << path;
        }
    }
}

TEST_P(GoNetHttpFSModes, FileServerZeroByte) {
    auto& ts = serve(http::FileServer(http::Dir(".")));
    auto req = newRequest("GET", ts.URL);
    req->URL.Path = string(std::string_view("/..\0", 4));
    auto [res, body] = doRequest(ts.Client(), req);
    ASSERT_TRUE(res);
    EXPECT_NE(res->StatusCode, 200) << "got status 200; want an error";
}

TEST_P(GoNetHttpFSModes, FileServerNullByte) {
    auto& ts = serve(http::FileServer(http::Dir("testdata")));
    for (std::string_view path : {"/file%00", "/%00", "/file/qwe/%00"}) {
        auto [res, body] = get(ts.Client(), str(ts.URL + path));
        ASSERT_TRUE(res);
        EXPECT_EQ(res->StatusCode, 404) << "Get(" << path << ")";
    }
}

TEST_P(GoNetHttpFSModes, FileServerNamesEscape) {
    auto& ts = serve(http::FileServer(http::Dir("testdata")));
    for (std::string_view path : {"/../testdata/file", "/NUL"}) {
        auto [res, body] = get(ts.Client(), str(ts.URL + path));
        ASSERT_TRUE(res);
        EXPECT_TRUE(res->StatusCode >= 400 && res->StatusCode <= 599)
            << "Get(" << path << "): got status " << res->StatusCode << ", want 4xx or 5xx";
    }
}

TEST_P(GoNetHttpFSModes, DirectoryIfNotModified) {
    constexpr std::string_view indexContents = "I am a fake index.html file";
    auto fileMod = time::Time::Unix(1000000000, 0);
    string fileModStr = fileMod.FormatHTTP();
    auto dirMod = time::Time::Unix(123, 0);
    auto indexFile = fakeInfo("index.html", fileMod, std::string(indexContents));
    auto fsys = make_ptr<fakeFs>();
    fsys->files["/"] = fakeInfo("", dirMod, "", true, {indexFile});
    fsys->files["/index.html"] = indexFile;

    auto modDone = make_chan<empty>();
    auto fsHandler = http::FileServer(fsys);
    auto& ts = serve([&](http::ResponseWriter& w, http::Request& r) {
        fsHandler->ServeHTTP(w, r);
        // After the first "If-Modified-Since" request, advance the
        // /index.html file's modtime, but not the directory's.
        if (!r.Header.Get("If-Modified-Since").empty() && indexFile->modtime.Equal(fileMod)) {
            indexFile->modtime = indexFile->modtime.Add(time::Hour);
            modDone.close();
        }
    });

    // Initial fetch of /index.html.
    auto [res, body] = get(ts.Client(), ts.URL);
    ASSERT_TRUE(res);
    ASSERT_EQ(body, indexContents);
    string lastMod = res->Header.Get("Last-Modified");
    ASSERT_EQ(lastMod, fileModStr) << "initial Last-Modified";

    // Fetch /index.html when it has not been modified.
    auto req = newRequest("GET", ts.URL);
    req->Header.Set("If-Modified-Since", lastMod);
    auto r2 = doRequest(ts.Client(), req);
    ASSERT_TRUE(r2.resp);
    ASSERT_EQ(r2.resp->StatusCode, 304) << "Code after If-Modified-Since request";

    // Fetch /index.html after it has been modified.
    modDone.recv();
    auto r3 = doRequest(ts.Client(), req);
    ASSERT_TRUE(r3.resp);
    ASSERT_EQ(r3.resp->StatusCode, 200) << "Code after second If-Modified-Since request";
}

namespace {

// A Reader and Seeker, as ServeContent takes: one of the test's contents.
struct readSeeker final : public io::ReadSeeker {
    ptr<io::Reader> r;
    io::Seeker* s = nullptr;
    ptr<object> keep;  // what s points into
    bool panicOnSeek = false;

    std::pair<isize, gopp::error> Read(std::span<byte> p) override { return r->Read(p); }
    std::pair<int64, gopp::error> Seek(int64 offset, int whence) override {
        if (panicOnSeek) ADD_FAILURE() << "Seek called on panicOnSeek";
        return s ? s->Seek(offset, whence) : std::pair<int64, gopp::error>(0, errors::New("no seeker"));
    }
};

ptr<readSeeker> rsOfFile(ptr<os::File> f) {
    auto rs = make_ptr<readSeeker>();
    rs->r = f;
    rs->s = f.get();
    rs->keep = f;
    return rs;
}

ptr<readSeeker> rsOfString(std::string_view s) {
    auto sr = strings::NewReader(string(s));
    auto rs = make_ptr<readSeeker>();
    rs->r = sr;
    rs->s = sr.get();
    rs->keep = sr;
    return rs;
}

}  // namespace

TEST_P(GoNetHttpFSModes, ServeContent) {
    struct serveParam {
        std::string name;
        time::Time modtime;
        ptr<readSeeker> content;
        std::string contentType;
        std::string etag;
    };
    auto servec = make_chan<serveParam>(1);
    auto& ts = serve([servec](http::ResponseWriter& w, http::Request& r) {
        serveParam p = servec.recv();
        if (!p.etag.empty()) w.Header().Set("ETag", string(p.etag));
        if (!p.contentType.empty()) w.Header().Set("Content-Type", string(p.contentType));
        http::ServeContent(w, r, p.name, p.modtime, *p.content);
    });

    struct testCase {
        // One of file or content must be set:
        std::string file{};
        std::optional<std::string> content{};
        bool panicOnSeek = false;

        time::Time modtime{};
        std::string serveEtag{};          // optional
        std::string serveContentType{};  // optional
        std::map<std::string, std::string> reqHeader{};
        std::string wantLastMod{};
        std::string wantContentType{};
        std::string wantContentRange{};
        int wantStatus = 0;
    };
    auto [hfi, herr] = os::Stat("testdata/index.html");
    ASSERT_FALSE(herr);
    time::Time htmlModTime = hfi->ModTime();
    std::string htmlMod = str(htmlModTime.FormatHTTP());

    std::map<std::string, testCase> tests;
    tests["no_last_modified"] = {.file = "testdata/style.css", .wantContentType = "text/css; charset=utf-8",
                                 .wantStatus = 200};
    tests["with_last_modified"] = {.file = "testdata/index.html", .modtime = htmlModTime, .wantLastMod = htmlMod,
                                   .wantContentType = "text/html; charset=utf-8", .wantStatus = 200};
    tests["not_modified_modtime"] = {.file = "testdata/style.css", .modtime = htmlModTime,
                                     .serveEtag = R"("foo")",  // Last-Modified sent only when no ETag
                                     .reqHeader = {{"If-Modified-Since", htmlMod}}, .wantStatus = 304};
    tests["not_modified_modtime_with_contenttype"] = {
        .file = "testdata/style.css", .modtime = htmlModTime, .serveEtag = R"("foo")",
        .serveContentType = "text/css",  // explicit content type
        .reqHeader = {{"If-Modified-Since", htmlMod}}, .wantStatus = 304};
    tests["not_modified_etag"] = {.file = "testdata/style.css", .serveEtag = R"("foo")",
                                  .reqHeader = {{"If-None-Match", R"("foo")"}}, .wantStatus = 304};
    tests["not_modified_etag_no_seek"] = {.content = "", .panicOnSeek = true,  // should never be called
                                          .serveEtag = R"(W/"foo")",  // If-None-Match uses weak ETag comparison
                                          .reqHeader = {{"If-None-Match", R"("baz", W/"foo")"}}, .wantStatus = 304};
    tests["if_none_match_mismatch"] = {.file = "testdata/style.css", .serveEtag = R"("foo")",
                                       .reqHeader = {{"If-None-Match", R"("Foo")"}},
                                       .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["if_none_match_malformed"] = {.file = "testdata/style.css", .serveEtag = R"("foo")",
                                        .reqHeader = {{"If-None-Match", ","}},
                                        .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["range_good"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                           .reqHeader = {{"Range", "bytes=0-4"}}, .wantContentType = "text/css; charset=utf-8",
                           .wantContentRange = "bytes 0-4/8", .wantStatus = http::StatusPartialContent};
    tests["range_match"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                            .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", R"("A")"}},
                            .wantContentType = "text/css; charset=utf-8", .wantContentRange = "bytes 0-4/8",
                            .wantStatus = http::StatusPartialContent};
    tests["range_match_weak_etag"] = {.file = "testdata/style.css", .serveEtag = R"(W/"A")",
                                      .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", R"(W/"A")"}},
                                      .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["range_no_overlap"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                                 .reqHeader = {{"Range", "bytes=10-20"}},
                                 .wantContentType = "text/plain; charset=utf-8", .wantContentRange = "bytes */8",
                                 .wantStatus = http::StatusRequestedRangeNotSatisfiable};
    // An If-Range resource for entity "A", but entity "B" is now current.
    // The Range request should be ignored.
    tests["range_no_match"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                               .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", R"("B")"}},
                               .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["range_with_modtime"] = {.file = "testdata/style.css", .modtime = date(2014, 6, 25, 17, 12, 18, 0),
                                   .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", "Wed, 25 Jun 2014 17:12:18 GMT"}},
                                   .wantLastMod = "Wed, 25 Jun 2014 17:12:18 GMT",
                                   .wantContentType = "text/css; charset=utf-8", .wantContentRange = "bytes 0-4/8",
                                   .wantStatus = http::StatusPartialContent};
    tests["range_with_modtime_mismatch"] = {
        .file = "testdata/style.css", .modtime = date(2014, 6, 25, 17, 12, 18, 0),
        .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", "Wed, 25 Jun 2014 17:12:19 GMT"}},
        .wantLastMod = "Wed, 25 Jun 2014 17:12:18 GMT", .wantContentType = "text/css; charset=utf-8",
        .wantStatus = http::StatusOK};
    tests["range_with_modtime_nanos"] = {
        .file = "testdata/style.css", .modtime = date(2014, 6, 25, 17, 12, 18, 123),
        .reqHeader = {{"Range", "bytes=0-4"}, {"If-Range", "Wed, 25 Jun 2014 17:12:18 GMT"}},
        .wantLastMod = "Wed, 25 Jun 2014 17:12:18 GMT", .wantContentType = "text/css; charset=utf-8",
        .wantContentRange = "bytes 0-4/8", .wantStatus = http::StatusPartialContent};
    tests["unix_zero_modtime"] = {.content = "<html>foo", .modtime = time::Time::Unix(0, 0),
                                  .wantContentType = "text/html; charset=utf-8", .wantStatus = http::StatusOK};
    tests["ifmatch_matches"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                                .reqHeader = {{"If-Match", R"("Z", "A")"}},
                                .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["ifmatch_star"] = {.file = "testdata/style.css", .serveEtag = R"("A")", .reqHeader = {{"If-Match", "*"}},
                             .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["ifmatch_failed"] = {.file = "testdata/style.css", .serveEtag = R"("A")",
                               .reqHeader = {{"If-Match", R"("B")"}}, .wantStatus = 412};
    tests["ifmatch_fails_on_weak_etag"] = {.file = "testdata/style.css", .serveEtag = R"(W/"A")",
                                           .reqHeader = {{"If-Match", R"(W/"A")"}}, .wantStatus = 412};
    tests["if_unmodified_since_true"] = {.file = "testdata/style.css", .modtime = htmlModTime,
                                         .reqHeader = {{"If-Unmodified-Since", htmlMod}}, .wantLastMod = htmlMod,
                                         .wantContentType = "text/css; charset=utf-8", .wantStatus = 200};
    tests["if_unmodified_since_false"] = {
        .file = "testdata/style.css", .modtime = htmlModTime,
        .reqHeader = {{"If-Unmodified-Since", str(htmlModTime.Add(-2 * time::Second).FormatHTTP())}},
        .wantLastMod = htmlMod, .wantStatus = 412};

    for (const auto& [testName, tt] : tests) {
        for (std::string_view method : {"GET", "HEAD"}) {
            ptr<readSeeker> content;
            if (!tt.file.empty()) {
                auto [f, err] = os::Open(tt.file);
                ASSERT_FALSE(err) << "test " << testName << ": " << err;
                content = rsOfFile(f);
            } else {
                content = rsOfString(*tt.content);
            }
            content->panicOnSeek = tt.panicOnSeek;
            servec.send(serveParam{
                .name = str(path::Base(tt.file)),
                .modtime = tt.modtime,
                .content = content,
                .contentType = tt.serveContentType,
                .etag = tt.serveEtag,
            });
            auto req = newRequest(method, ts.URL);
            for (const auto& [k, v] : tt.reqHeader) req->Header.Set(k, string(v));
            auto [res, body] = doRequest(ts.Client(), req);
            if (auto* f = dynamic_cast<os::File*>(content->keep.get())) f->Close();
            ASSERT_TRUE(res);
            EXPECT_EQ(res->StatusCode, tt.wantStatus) << "test " << testName << " using " << method;
            EXPECT_EQ(res->Header.Get("Content-Type"), tt.wantContentType)
                << "test " << testName << " using " << method;
            EXPECT_EQ(res->Header.Get("Content-Range"), tt.wantContentRange)
                << "test " << testName << " using " << method;
            EXPECT_EQ(res->Header.Get("Last-Modified"), tt.wantLastMod) << "test " << testName << " using " << method;
        }
    }
}

namespace {

// issue12991FS: every file's Stat fails with ErrPermission.
class issue12991File final : public http::File {
public:
    std::pair<isize, gopp::error> Read(std::span<byte>) override { return {0, io::EOF_}; }
    std::pair<int64, gopp::error> Seek(int64, int) override { return {0, nullptr}; }
    gopp::error Close() override { return nullptr; }
    std::pair<std::vector<ptr<fs::FileInfo>>, gopp::error> Readdir(int) override { return {{}, nullptr}; }
    std::pair<ptr<fs::FileInfo>, gopp::error> Stat() override { return {nullptr, fs::ErrPermission}; }
};

}  // namespace

// Issue 12991. (Go calls serveFile directly; FileServer stats the file
// before anything else too.)
TEST_F(GoNetHttpFS, ServerFileStatError) {
    auto rec = httptest::NewRecorder();
    auto req = newRequest("GET", "http://foo/file.txt");
    auto fsys = make_ptr<testFileSystem>(
        [](std::string_view) { return std::pair<ptr<http::File>, gopp::error>(make_ptr<issue12991File>(), nullptr); });
    http::FileServer(fsys)->ServeHTTP(*rec, *req);
    EXPECT_TRUE(rec->Body.find("403") != std::string::npos && rec->Body.find("Forbidden") != std::string::npos)
        << "wanted 403 forbidden message; got: " << rec->Body;
}

TEST_P(GoNetHttpFSModes, FileServerErrorMessages) {
    auto fsys = make_ptr<fakeFs>();
    auto e500 = make_ptr<fakeFileInfo>();
    e500->err = errors::New("random error");
    auto e403 = make_ptr<fakeFileInfo>();
    e403->err = make_ptr<fs::PathError>(string(), string(), fs::ErrPermission);
    fsys->files["/500"] = e500;
    fsys->files["/403"] = e403;
    auto server = http::FileServer(fsys);
    auto& ts = serve([server](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Etag", string::fromStatic("étude"));
        w.Header().Set("Cache-Control", string::fromStatic("yes"));
        w.Header().Set("Content-Type", string::fromStatic("awesome"));
        w.Header().Set("Last-Modified", string::fromStatic("yesterday"));
        server->ServeHTTP(w, r);
    });
    for (int code : {403, 404, 500}) {
        auto [res, body] = get(ts.Client(), fmt::Sprint("{}/{}", ts.URL, code).view());
        if (!res) continue;
        EXPECT_EQ(res->StatusCode, code) << "GET /" << code;
        for (std::string_view hdr : {"Etag", "Last-Modified", "Cache-Control"}) {
            EXPECT_FALSE(res->Header.Has(hdr)) << "GET /" << code << ": Header[" << hdr << "] = "
                                               << res->Header.Get(hdr) << ", want not present";
        }
    }
}

TEST_P(GoNetHttpFSModes, FileServerNotDirError) {
    auto [wd, werr] = os::Getwd();
    ASSERT_FALSE(werr);
    for (int variant = 0; variant < 2; variant++) {
        SCOPED_TRACE(variant == 0 ? "Dir" : "FS");
        auto newfs = [variant](std::string p) -> ptr<http::FileSystem> {
            if (variant == 0) return make_ptr<http::Dir>(string(p));
            return http::FS(os::DirFS(p));
        };
        auto& ts = serve(http::FileServer(newfs("testdata")));
        auto [res, body] = get(ts.Client(), str(ts.URL + "/index.html/not-a-file"));
        ASSERT_TRUE(res);
        EXPECT_EQ(res->StatusCode, 404);

        auto test = [](std::string_view name, const ptr<http::FileSystem>& fsys) {
            SCOPED_TRACE(name);
            for (std::string_view p : {"/index.html/not-a-file", "/index.html/not-a-dir/not-a-file"}) {
                auto [f, err] = fsys->Open(p);
                ASSERT_TRUE(err) << "err == nil; want != nil";
                EXPECT_TRUE(errors::Is(err, fs::ErrNotExist)) << "err = " << err << "; want fs::ErrNotExist";
            }
        };
        test("RelativePath", newfs("testdata"));
        test("AbsolutePath", newfs(os_test::join(str(wd), "testdata")));
    }
}

namespace {

// fileServerCleanPathDir: logs the names opened.
class fileServerCleanPathDir final : public http::FileSystem {
public:
    explicit fileServerCleanPathDir(std::vector<std::string>* log) : log_(log) {}
    std::pair<ptr<http::File>, gopp::error> Open(std::string_view p) override {
        log_->emplace_back(p);
        // Just return back something that's a directory.
        if (p == "/" || p == "/dir" || p == "/dir/") return http::Dir(".").Open(".");
        return {nullptr, fs::ErrNotExist};
    }

private:
    std::vector<std::string>* log_;
};

}  // namespace

TEST_F(GoNetHttpFS, FileServerCleanPath) {
    struct {
        std::string path;
        int wantCode;
        std::vector<std::string> wantOpen;
    } tests[] = {
        {"/", 200, {"/", "/index.html"}},
        {"/dir", 301, {"/dir"}},
        {"/dir/", 200, {"/dir", "/dir/index.html"}},
    };
    for (const auto& tt : tests) {
        std::vector<std::string> log;
        auto rr = httptest::NewRecorder();
        auto req = newRequest("GET", "http://foo.localhost" + tt.path);
        http::FileServer(make_ptr<fileServerCleanPathDir>(&log))->ServeHTTP(*rr, *req);
        EXPECT_EQ(log, tt.wantOpen) << "For " << tt.path << ": Opens";
        EXPECT_EQ(rr->Code, tt.wantCode) << "For " << tt.path << ": Response code";
    }
}

TEST(GoNetHttp, ScanETag) {
    struct {
        std::string in, wantEtag, wantRemain;
    } tests[] = {
        {R"(W/"etag-1")", R"(W/"etag-1")", ""},
        {R"("etag-2")", R"("etag-2")", ""},
        {R"("etag-1", "etag-2")", R"("etag-1")", R"(, "etag-2")"},
        {"", "", ""},
        {"W/", "", ""},
        {R"(W/"truc)", "", ""},
        {R"(w/"case-sensitive")", "", ""},
        {R"("spaced etag")", "", ""},
    };
    for (const auto& test : tests) {
        auto [etag, remain] = http::internal::scanEtag(test.in);
        EXPECT_EQ(etag, test.wantEtag) << "scanETag(" << test.in << ")";
        EXPECT_EQ(remain, test.wantRemain) << "scanETag(" << test.in << ")";
    }
}

// Issue 40940: Ensure that we only accept non-negative suffix-lengths in
// "Range": "bytes=-N", and should reject "bytes=--2".
TEST_P(GoNetHttpFSModes, ServeFileRejectsInvalidSuffixLengths) {
    auto& ts = serve(http::FileServer(http::Dir("testdata")));
    struct {
        std::string r;
        int wantCode;
        std::string wantBody;
    } tests[] = {
        {"bytes=--6", 416, "invalid range\n"},
        {"bytes=--0", 416, "invalid range\n"},
        {"bytes=---0", 416, "invalid range\n"},
        {"bytes=-6", 206, "hello\n"},
        {"bytes=6-", 206, "html says hello\n"},
        {"bytes=-6-", 416, "invalid range\n"},
        {"bytes=-0", 206, ""},
        {"bytes=", 200, "index.html says hello\n"},
    };
    for (const auto& tt : tests) {
        auto req = newRequest("GET", str(ts.URL + "/index.html"));
        req->Header.Set("Range", string(tt.r));
        auto [res, body] = doRequest(ts.Client(), req);
        ASSERT_TRUE(res);
        EXPECT_EQ(res->StatusCode, tt.wantCode) << tt.r;
        EXPECT_EQ(body, tt.wantBody) << tt.r;
    }
}

TEST_P(GoNetHttpFSModes, FileServerMethods) {
    auto& ts = serve(http::FileServer(http::Dir("testdata")));
    std::string file = readFile(testFile);
    // Get contents via various methods. See https://go.dev/issue/59471 for
    // a proposal to limit the set of methods handled. For now, test the
    // historical behavior.
    for (std::string_view method : {http::MethodGet, http::MethodHead, http::MethodPost, http::MethodPut,
                                    http::MethodPatch, http::MethodDelete, http::MethodOptions, http::MethodTrace}) {
        auto [res, body] = doRequest(ts.Client(), newRequest(method, str(ts.URL + "/file")));
        ASSERT_TRUE(res);
        ASSERT_EQ(body, method == http::MethodHead ? "" : file) << method;
        ASSERT_EQ(res->Header.Get("Content-Length"), fmt::Sprint("{}", file.size())) << method;
    }
}

TEST_F(GoNetHttpFS, FileServerFS) {
    constexpr std::string_view filename = "index.html";
    constexpr std::string_view contents = "index.html says hello";
    auto fsys = make_ptr<fstest::MapFS>();
    (*fsys)[filename].Data = toBytes(contents);
    auto ts = httptest::NewServer(http::FileServerFS(fsys));
    auto [res, body] = get(ts->Client(), str(ts->URL + "/" + filename));
    EXPECT_EQ(body, contents) << "for path " << filename;
    ts->Close();
}

TEST_F(GoNetHttpFS, ServeFileFS) {
    constexpr std::string_view filename = "index.html";
    constexpr std::string_view contents = "index.html says hello";
    auto fsys = make_ptr<fstest::MapFS>();
    (*fsys)[filename].Data = toBytes(contents);
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>(
        [fsys, filename](http::ResponseWriter& w, http::Request& r) { http::ServeFileFS(w, r, fsys, filename); }));
    auto [res, body] = get(ts->Client(), str(ts->URL + "/" + filename));
    EXPECT_EQ(body, contents) << "for path " << filename;
    ts->Close();
}

// Issue 63769
TEST_P(GoNetHttpFSModes, FileServerDirWithRootFile) {
    for (int variant = 0; variant < 2; variant++) {
        SCOPED_TRACE(variant == 0 ? "FileServer" : "FileServerFS");
        auto h = variant == 0 ? http::FileServer(http::Dir("testdata/index.html"))
                              : http::FileServerFS(os::DirFS("testdata/index.html"));
        auto& ts = serve(h);
        auto [res, body] = get(ts.Client(), ts.URL);
        ASSERT_TRUE(res);
        EXPECT_EQ(res->StatusCode, http::StatusInternalServerError);
    }
}

TEST_F(GoNetHttpFS, ServeContentHeadersWithError) {
    constexpr std::string_view contents = "content";
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>([&](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", string::fromStatic("application/octet-stream"));
        w.Header().Set("Content-Length", strconv::Itoa(static_cast<int64>(contents.size())));
        w.Header().Set("Content-Encoding", string::fromStatic("gzip"));
        w.Header().Set("Etag", string::fromStatic(R"("abcdefgh")"));
        w.Header().Set("Last-Modified", string::fromStatic("Wed, 21 Oct 2015 07:28:00 GMT"));
        w.Header().Set("Cache-Control", string::fromStatic("immutable"));
        w.Header().Set("Other-Header", string::fromStatic("test"));
        auto content = strings::NewReader(string(contents));
        http::ServeContent(w, r, "", time::Time{}, *content);
    }));
    auto req = newRequest("GET", ts->URL);
    req->Header.Set("Range", string::fromStatic("bytes=100-10000"));
    auto [res, out] = doRequest(ts->Client(), req);
    ts->Close();
    ASSERT_TRUE(res);

    EXPECT_EQ(res->StatusCode, 416);
    EXPECT_EQ(out, "invalid range: failed to overlap\n");
    EXPECT_EQ(res->Header.Get("Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(res->Header.Get("Content-Length"), fmt::Sprint("{}", out.size()));
    EXPECT_EQ(res->Header.Get("Content-Encoding"), "");
    EXPECT_EQ(res->Header.Get("Etag"), "");
    EXPECT_EQ(res->Header.Get("Last-Modified"), "");
    EXPECT_EQ(res->Header.Get("Cache-Control"), "");
    EXPECT_EQ(res->Header.Get("Content-Range"), "bytes */7");
    EXPECT_EQ(res->Header.Get("Other-Header"), "test");
}

// http.ParseTime: the three formats, and errors.
TEST(GoNetHttp, ParseTime) {
    auto want = date(1994, 11, 6, 8, 49, 37, 0);
    for (std::string_view s : {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
                               "Sun Nov  6 08:49:37 1994"}) {
        auto [t, err] = http::ParseTime(s);
        ASSERT_FALSE(err) << s << ": " << err;
        EXPECT_EQ(t.UnixNano(), want.UnixNano()) << s;
    }
    auto t = date(2014, 6, 25, 17, 12, 18, 0);
    EXPECT_EQ(t.FormatHTTP(), "Wed, 25 Jun 2014 17:12:18 GMT");
    EXPECT_EQ(http::ParseTime(t.FormatHTTP()).first.Unix(), t.Unix());
    for (std::string_view s : {"", "Sun, 06 Nov 1994 08:49:37", "Sun, 31 Nov 1994 08:49:37 GMT",
                               "Sun, 06 Nov 1994 24:49:37 GMT", "Sun, 06 Nov 1994 08:49:37 GMT extra", "yesterday"}) {
        EXPECT_TRUE(http::ParseTime(s).second) << s;
    }
}

namespace {

// TestLinuxSendfileChild: serves os::TempDir() on the listener at
// descriptor 3, until a request for /quit.
void sendfileChild(const std::vector<std::string>&) {
    auto [ln, err] = net::FileListener(os::NewFile(3, string::fromStatic("ephemeral-port-listener")));
    if (err) panic(err.Error());
    auto mux = make_ptr<http::ServeMux>();
    mux->Handle("/", http::FileServer(http::Dir(os::TempDir())));
    mux->HandleFunc("/quit", [](http::ResponseWriter&, http::Request&) { os::Exit(0); });
    http::Server s;
    s.Handler = mux;
    if (auto serr = s.Serve(ln)) panic(serr.Error());
}

const bool sendfileChildRegistered = go_helper::registerHelper("sendfilechild", sendfileChild);

}  // namespace

// The file goes to the connection with sendfile(2): the server runs under
// strace, which shows the call.
TEST(GoNetHttp, LinuxSendfile) {
    auto [strace, lerr] = exec::LookPath("strace");
    if (lerr) GTEST_SKIP() << "skipping; strace not found in path";

    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    auto [lnf, ferr] = static_cast<net::TCPListener&>(*ln).File();
    ASSERT_FALSE(ferr) << ferr;

    // Attempt to run strace, and skip on failure: this test requires
    // SYS_PTRACE.
    if (auto serr = exec::Command(strace.view(), "-f", "-q", "true")->Run()) {
        GTEST_SKIP() << "skipping; failed to run strace: " << serr;
    }

    string filename = fmt::Sprint("1kb-{}", os::Getpid());
    string filepath = path::Join(os::TempDir(), filename);
    ASSERT_FALSE(os::WriteFile(filepath, toBytes(std::string(1 << 10, 'a')), 0755));

    auto child = go_helper::helperCommand("sendfilechild");
    child->Args = {strace, string::fromStatic("-f"), string::fromStatic("-q"), child->Path,
                   string::fromStatic("sendfilechild")};
    child->Path = strace;
    child->ExtraFiles = {lnf};
    auto buf = make_ptr<strings::Builder>();
    child->Stdout = buf;
    child->Stderr = buf;
    if (auto serr = child->Start()) {
        os::Remove(filepath);
        GTEST_SKIP() << "skipping; failed to start straced child: " << serr;
    }

    // A timeout, which Go's test doesn't have: if the child is gone, the
    // listener here keeps a request waiting.
    http::Client client;
    client.Timeout = 10 * time::Second;
    string addr = ln->Addr()->String();
    auto [res, gerr] = client.Get(str(fmt::Sprint("http://{}/{}", addr, filename)));
    EXPECT_FALSE(gerr) << "http client error: " << gerr;
    if (res) {
        auto [body, rerr] = io::ReadAll(*res->Body);
        EXPECT_FALSE(rerr) << "client body read error: " << rerr;
        EXPECT_EQ(body.size(), std::size_t(1) << 10);
        res->Body->Close();
    }

    // Force the child to exit cleanly. A POST, which isn't sent again on
    // another connection when this one closes, as the child exits.
    if (auto [qres, qerr] = client.Post(str(fmt::Sprint("http://{}/quit", addr)), "", nullptr); qres) {
        qres->Body->Close();
    }
    child->Wait();
    os::Remove(filepath);
    ln->Close();

    std::string_view out = buf->view();
    EXPECT_TRUE(out.find("sendfile(") != out.npos || out.find("sendfile64(") != out.npos)
        << "no sendfile system call found in:\n"
        << out;
}
