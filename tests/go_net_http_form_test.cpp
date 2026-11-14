// Ported from Go 1.27.3's net/http/request_test.go: the tests of forms,
// multipart requests and MaxBytesReader, with Go's test names. go++ also
// tests an upload through a server, and PostForm.
#include <gopp/bytes.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/mime/multipart.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    std::string r;
    std::size_t i = 0;
    for (;;) {
        auto j = s.find(from, i);
        if (j == std::string::npos) break;
        r.append(s, i, j - i);
        r += to;
        i = j + from.size();
    }
    r.append(s, i);
    return r;
}

// The values of v as a map, as Go's url.Values is, to compare.
std::map<std::string, std::vector<std::string>> asMap(const url::Values& v) {
    std::map<std::string, std::vector<std::string>> m;
    for (const auto& [k, val] : v) m[std::string(k.view())].push_back(std::string(val.view()));
    return m;
}

std::vector<std::string> strs(const std::vector<string>& v) {
    std::vector<std::string> r;
    for (const auto& s : v) r.push_back(std::string(s.view()));
    return r;
}

ptr<http::Request> newRequest(std::string_view method, std::string_view url, std::string body) {
    auto [req, err] = http::NewRequest(method, url, strings::NewReader(string(body)));
    EXPECT_FALSE(err) << err;
    return req;
}

// A Request as a Go test builds one: &Request{Method, Header, Body}.
ptr<http::Request> rawRequest(std::string method, std::string contentType, ptr<io::Reader> body) {
    auto req = make_ptr<http::Request>();
    req->Method = string(method);
    if (!contentType.empty()) req->Header.Set("Content-Type", string(contentType));
    req->Body = io::NopCloser(std::move(body));
    return req;
}

TEST(GoNetHttpForm, Query) {
    auto req = make_ptr<http::Request>();
    req->Method = "GET";
    req->URL = url::Parse("http://www.google.com/search?q=foo&q=bar").first;
    EXPECT_EQ(req->FormValue("q"), "foo");
}

// Issue #25192: Test that ParseForm fails but still parses the form when a
// URL containing a semicolon is provided.
TEST(GoNetHttpForm, ParseFormSemicolonSeparator) {
    for (std::string method : {"POST", "PATCH", "PUT", "GET"}) {
        auto req = newRequest(method, "http://www.google.com/search?q=foo;q=bar&a=1", "q");
        auto err = req->ParseForm();
        ASSERT_TRUE(err) << "for method " << method << ", ParseForm expected an error, got success";
        decltype(asMap(req->Form)) wantForm = {{"a", {"1"}}};
        ASSERT_EQ(asMap(req->Form), wantForm) << "for method " << method;
    }
}

TEST(GoNetHttpForm, ParseFormQuery) {
    auto req = newRequest("POST", "http://www.google.com/search?q=foo&q=bar&both=x&prio=1&orphan=nope&empty=not",
                          "z=post&both=y&prio=2&=nokey&orphan&empty=&");
    req->Header.Set("Content-Type", "application/x-www-form-urlencoded; param=value");

    EXPECT_EQ(req->FormValue("q"), "foo");
    EXPECT_EQ(req->FormValue("z"), "post");
    EXPECT_FALSE(req->PostForm.Has("q")) << R"(req.PostForm["q"], want no entry in map)";
    EXPECT_EQ(req->PostFormValue("z"), "post");
    EXPECT_EQ(strs(req->Form["q"]), (std::vector<std::string>{"foo", "bar"}));
    EXPECT_EQ(strs(req->Form["both"]), (std::vector<std::string>{"y", "x"}));
    EXPECT_EQ(req->FormValue("prio"), "2") << "from body";
    EXPECT_EQ(strs(req->Form["orphan"]), (std::vector<std::string>{"", "nope"}));
    EXPECT_EQ(strs(req->Form["empty"]), (std::vector<std::string>{"", "not"}));
    EXPECT_EQ(strs(req->Form[""]), (std::vector<std::string>{"nokey"}));
}

// Tests that we only parse the form automatically for certain methods.
TEST(GoNetHttpForm, ParseFormQueryMethods) {
    for (std::string method : {"POST", "PATCH", "PUT", "FOO"}) {
        auto req = newRequest(method, "http://www.google.com/search", "foo=bar");
        req->Header.Set("Content-Type", "application/x-www-form-urlencoded; param=value");
        std::string want = method == "FOO" ? "" : "bar";
        EXPECT_EQ(req->FormValue("foo"), want) << "for method " << method;
    }
}

TEST(GoNetHttpForm, ParseFormUnknownContentType) {
    struct test {
        std::string name, wantErr, contentType;
    };
    for (const auto& tt : std::vector<test>{
             {"text", "", "text/plain"},
             // Empty content type is legal - may be treated as
             // application/octet-stream (RFC 7231, section 3.1.1.5)
             {"empty", "", ""},
             {"boundary", "mime: invalid media parameter", "text/plain; boundary="},
             {"unknown", "", "application/unknown"},
         }) {
        auto req = rawRequest("POST", tt.contentType, strings::NewReader("body"));
        auto err = req->ParseForm();
        if (tt.wantErr.empty()) {
            EXPECT_FALSE(err) << tt.name << ": want success, got error: " << err;
        } else {
            EXPECT_EQ(err.Error(), tt.wantErr) << tt.name;
        }
    }
}

TEST(GoNetHttpForm, ParseFormInitializeOnError) {
    auto [nilBody, _] = http::NewRequest("POST", "http://www.google.com/search?q=foo", nullptr);
    auto get = make_ptr<http::Request>();
    get->Method = "GET";
    for (auto& req : {nilBody, get}) {
        // Go checks that Form and PostForm aren't nil; here, that
        // ParseForm parses, and doesn't again.
        req->ParseForm();
        req->Form.Add("x", "y");
        req->ParseForm();
        EXPECT_EQ(req->Form.Get("x"), "y");
    }
}

TEST(GoNetHttpForm, MultipartReader) {
    struct test {
        bool shouldError;
        std::string contentType;
    };
    int i = 0;
    for (const auto& tt : std::vector<test>{
             {false, R"(multipart/form-data; boundary="foo123")"},
             {false, R"(multipart/mixed; boundary="foo123")"},
             {true, "text/plain"},
         }) {
        auto req = rawRequest("POST", tt.contentType, make_ptr<bytes::Buffer>());
        auto [mr, err] = req->MultipartReader();
        if (tt.shouldError) {
            EXPECT_TRUE(err && !mr) << "test " << i;
        } else {
            EXPECT_TRUE(!err && mr) << "test " << i << ": " << err;
        }
        i++;
    }
}

// Issue 9305: ParseMultipartForm should populate PostForm too.
TEST(GoNetHttpForm, ParseMultipartFormPopulatesPostForm) {
    std::string postData = R"(--xxx
Content-Disposition: form-data; name="field1"

value1
--xxx
Content-Disposition: form-data; name="field2"

value2
--xxx
Content-Disposition: form-data; name="file"; filename="file"
Content-Type: application/octet-stream
Content-Transfer-Encoding: binary

binary data
--xxx--
)";
    auto req = rawRequest("POST", "multipart/form-data; boundary=xxx", strings::NewReader(string(postData)));
    for (auto [k, v] : std::vector<std::pair<std::string, std::string>>{
             {"language", "Go"}, {"name", "gopher"}, {"skill", "go-ing"}, {"field2", "initial-value2"}}) {
        req->Form.Add(string(k), string(v));
    }
    ASSERT_FALSE(req->ParseMultipartForm(10000));
    decltype(asMap(req->Form)) wantForm = {
        {"language", {"Go"}}, {"name", {"gopher"}}, {"skill", {"go-ing"}},
        {"field1", {"value1"}}, {"field2", {"initial-value2", "value2"}},
    };
    EXPECT_EQ(asMap(req->Form), wantForm);
    decltype(asMap(req->Form)) wantPostForm = {{"field1", {"value1"}}, {"field2", {"value2"}}};
    EXPECT_EQ(asMap(req->PostForm), wantPostForm);
    req->MultipartForm->RemoveAll();
}

TEST(GoNetHttpForm, ParseMultipartForm) {
    auto req = rawRequest("POST", R"(multipart/form-data; boundary="foo123")", make_ptr<bytes::Buffer>());
    EXPECT_TRUE(req->ParseMultipartForm(25)) << "expected multipart EOF, got nil";
    req->Header.Set("Content-Type", "text/plain");
    EXPECT_EQ(req->ParseMultipartForm(25), http::ErrNotMultipart) << "expected ErrNotMultipart for text/plain";
}

// Issue 45789: multipart form should not include directory path in
// filename.
TEST(GoNetHttpForm, ParseMultipartFormFilename) {
    std::string postData = R"(--xxx
Content-Disposition: form-data; name="file"; filename="../usr/foobar.txt/"
Content-Type: text/plain

--xxx--
)";
    auto req = rawRequest("POST", "multipart/form-data; boundary=xxx", strings::NewReader(string(postData)));
    auto [_, hdr, err] = req->FormFile("file");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(hdr->Filename, "foobar.txt");
}

// Issue #40430: Test that if maxMemory for ParseMultipartForm when combined
// with the payload size and the internal leeway buffer size of 10MiB
// overflows, that we correctly return an error.
TEST(GoNetHttpForm, MaxInt64ForMultipartFormMaxMemoryOverflow) {
    constexpr int payloadSize = 1 << 10;
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>([](http::ResponseWriter& rw, http::Request& req) {
        // The combination of MaxInt64 + payloadSize + (internal spare of
        // 10MiB) triggers the overflow. See https://golang.org/issue/40430/
        if (auto err = req.ParseMultipartForm(std::numeric_limits<int64>::max())) {
            http::Error(rw, err.Error(), http::StatusBadRequest);
        }
    }));
    auto fBuf = make_ptr<bytes::Buffer>();
    auto mw = multipart::NewWriter(fBuf);
    auto [mf, err] = mw->CreateFormFile("file", "myfile.txt");
    ASSERT_FALSE(err);
    std::string payload;
    for (int i = 0; i < payloadSize; i++) payload += "abc";
    ASSERT_FALSE(io::WriteString(*mf, payload).second);
    ASSERT_FALSE(mw->Close());
    auto [req, rerr] = http::NewRequest("POST", ts->URL, fBuf);
    ASSERT_FALSE(rerr);
    req->Header.Set("Content-Type", mw->FormDataContentType());
    auto [res, derr] = ts->Client().Do(req);
    ASSERT_FALSE(derr) << derr;
    res->Body->Close();
    EXPECT_EQ(res->StatusCode, http::StatusOK);
    ts->Close();
}

const std::string fileaContents = "This is a test file.";
const std::string filebContents = "Another test file.";
const std::string textaValue = "foo";
const std::string textbValue = "bar";
const std::string boundary = "MyBoundary";

const std::string message = R"(
--MyBoundary
Content-Disposition: form-data; name="filea"; filename="filea.txt"
Content-Type: text/plain

)" + fileaContents + R"(
--MyBoundary
Content-Disposition: form-data; name="fileb"; filename="fileb.txt"
Content-Type: text/plain

)" + filebContents + R"(
--MyBoundary
Content-Disposition: form-data; name="texta"

)" + textaValue + R"(
--MyBoundary
Content-Disposition: form-data; name="textb"

)" + textbValue + R"(
--MyBoundary--
)";

ptr<http::Request> newTestMultipartRequest() {
    auto req = newRequest("POST", "/", replaceAll(message, "\n", "\r\n"));
    req->Header.Set("Content-type", fmt::Sprint(R"(multipart/form-data; boundary="{}")", boundary));
    return req;
}

void testMissingFile(http::Request& req) {
    auto [f, fh, err] = req.FormFile("missing");
    EXPECT_FALSE(f) << "FormFile file, want nil";
    EXPECT_FALSE(fh) << "FormFile file header, want nil";
    EXPECT_EQ(err, http::ErrMissingFile) << err;
}

ptr<multipart::File> testMultipartFile(http::Request& req, std::string key, std::string expectFilename,
                                       std::string expectContent) {
    auto [f, fh, err] = req.FormFile(key);
    if (err) {
        ADD_FAILURE() << "FormFile(" << key << "): " << err;
        return nullptr;
    }
    EXPECT_EQ(fh->Filename, expectFilename);
    strings::Builder b;
    EXPECT_FALSE(io::Copy(b, *f).second);
    EXPECT_EQ(b.String(), expectContent);
    return f;
}

// Go also checks whether the files are in memory (filea) or on disk
// (fileb, unless allMem): multipart::File doesn't tell.
void validateTestMultipartContents(http::Request& req) {
    EXPECT_EQ(req.FormValue("texta"), textaValue);
    EXPECT_EQ(req.FormValue("textb"), textbValue);
    EXPECT_EQ(req.FormValue("missing"), "");
    if (auto fda = testMultipartFile(req, "filea", "filea.txt", fileaContents)) fda->Close();
    if (auto fdb = testMultipartFile(req, "fileb", "fileb.txt", filebContents)) fdb->Close();
    testMissingFile(req);
}

TEST(GoNetHttpForm, MultipartRequest) {
    // Test that we can read the values and files of a multipart request
    // with FormValue and FormFile, and that ParseMultipartForm can be called
    // multiple times.
    auto req = newTestMultipartRequest();
    ASSERT_FALSE(req->ParseMultipartForm(25)) << "ParseMultipartForm first call";
    validateTestMultipartContents(*req);
    ASSERT_FALSE(req->ParseMultipartForm(25)) << "ParseMultipartForm second call";
    validateTestMultipartContents(*req);
    req->MultipartForm->RemoveAll();
}

// Issue #25192: Test that ParseMultipartForm fails but still parses the
// multi-part form when a URL containing a semicolon is provided.
TEST(GoNetHttpForm, ParseMultipartFormSemicolonSeparator) {
    auto req = newTestMultipartRequest();
    req->URL = url::URL{};
    req->URL.RawQuery = "q=foo;q=bar";
    ASSERT_TRUE(req->ParseMultipartForm(25)) << "ParseMultipartForm expected error due to invalid semicolon, got nil";
    validateTestMultipartContents(*req);
    req->MultipartForm->RemoveAll();
}

TEST(GoNetHttpForm, MultipartRequestAuto) {
    // Test that FormValue and FormFile automatically invoke
    // ParseMultipartForm and return the right values.
    auto req = newTestMultipartRequest();
    validateTestMultipartContents(*req);
    if (req->MultipartForm) req->MultipartForm->RemoveAll();
}

TEST(GoNetHttpForm, MissingFileMultipartRequest) {
    // Test that FormFile returns an error if the named file is missing.
    auto req = newTestMultipartRequest();
    testMissingFile(*req);
    if (req->MultipartForm) req->MultipartForm->RemoveAll();
}

// Test that FormValue invokes ParseMultipartForm.
TEST(GoNetHttpForm, FormValueCallsParseMultipartForm) {
    auto req = newRequest("POST", "http://www.google.com/", "z=post");
    req->Header.Set("Content-Type", "application/x-www-form-urlencoded; param=value");
    ASSERT_EQ(req->Form.size(), 0u) << "Unexpected request Form, want nil";
    req->FormValue("z");
    ASSERT_NE(req->Form.size(), 0u) << "ParseMultipartForm not called by FormValue";
}

// Test that FormFile invokes ParseMultipartForm.
TEST(GoNetHttpForm, FormFileCallsParseMultipartForm) {
    auto req = newTestMultipartRequest();
    ASSERT_EQ(req->Form.size(), 0u) << "Unexpected request Form, want nil";
    req->FormFile("");
    ASSERT_NE(req->Form.size(), 0u) << "ParseMultipartForm not called by FormFile";
    req->MultipartForm->RemoveAll();
}

// Test that ParseMultipartForm errors if called after MultipartReader on the
// same request.
TEST(GoNetHttpForm, ParseMultipartFormOrder) {
    auto req = newTestMultipartRequest();
    ASSERT_FALSE(req->MultipartReader().second);
    ASSERT_TRUE(req->ParseMultipartForm(1024)) << "expected an error from ParseMultipartForm after call to MultipartReader";
}

// Test that MultipartReader errors if called after ParseMultipartForm on the
// same request.
TEST(GoNetHttpForm, MultipartReaderOrder) {
    auto req = newTestMultipartRequest();
    ASSERT_FALSE(req->ParseMultipartForm(25));
    ASSERT_TRUE(req->MultipartReader().second) << "expected an error from MultipartReader after call to ParseMultipartForm";
    req->MultipartForm->RemoveAll();
}

// Test that FormFile errors if called after MultipartReader on the same
// request.
TEST(GoNetHttpForm, FormFileOrder) {
    auto req = newTestMultipartRequest();
    ASSERT_FALSE(req->MultipartReader().second);
    ASSERT_TRUE(std::get<2>(req->FormFile(""))) << "expected an error from FormFile after call to MultipartReader";
}

// delayedEOFReader never returns (n > 0, io.EOF), instead putting off the
// io.EOF until a subsequent Read call.
struct delayedEOFReader final : io::Reader {
    ptr<io::Reader> r;
    explicit delayedEOFReader(ptr<io::Reader> r_) : r(std::move(r_)) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        auto [n, err] = r->Read(p);
        if (n > 0 && err == io::EOF_) err = nullptr;
        return {n, err};
    }
};

TEST(GoNetHttpForm, Issue10884_MaxBytesEOF) {
    auto rc = http::MaxBytesReader(nullptr, io::NopCloser(make_ptr<delayedEOFReader>(strings::NewReader("12345"))), 5);
    auto [_, err] = io::Copy(io::Discard(), *rc);
    EXPECT_FALSE(err) << err;
}

// Issue 14981: MaxBytesReader's return error wasn't sticky. It doesn't
// technically need to be, but people expected it to be.
TEST(GoNetHttpForm, MaxBytesReaderStickyError) {
    auto isSticky = [](io::Reader& r) -> bool {
        byte buf[1000];
        error firstErr;
        for (;;) {
            auto [n, err] = r.Read(buf);
            if (!err) continue;
            if (!firstErr) {
                firstErr = err;
                continue;
            }
            return err == firstErr;
        }
    };
    struct test {
        int readable;
        int64 limit;
    };
    int i = 0;
    for (auto tt : std::vector<test>{{99, 100}, {100, 100}, {101, 100}}) {
        auto rc = http::MaxBytesReader(nullptr, io::NopCloser(bytes::NewReader(slice<byte>::make(std::size_t(tt.readable)))),
                                       tt.limit);
        EXPECT_TRUE(isSticky(*rc)) << i << ". non-sticky error";
        i++;
    }
}

// Issue 45101: maxBytesReader's Read panicked when n < -1. This test also
// ensures that Read treats negative limits as equivalent to 0.
TEST(GoNetHttpForm, MaxBytesReaderDifferentLimits) {
    const std::string testStr = "1234";
    struct test {
        int64 limit;
        std::size_t lenP;
        isize wantN;
        bool wantErr;
    };
    std::vector<test> tests = {
        {-123, 0, 0, false},  // no error when the limit is negative, but we don't need to read
        {-100, 32 * 1024, 0, true},
        {-2, 1, 0, true},
        {-1, 2, 0, true},
        {0, 3, 0, true},
        {1, 4, 1, true},
        {2, 5, 2, true},
        {3, 2, 2, false},
        {int64(testStr.size()), testStr.size(), isize(testStr.size()), false},
        {100, 6, isize(testStr.size()), false},
        {std::numeric_limits<int64>::max(), testStr.size(), isize(testStr.size()), false},  // Issue 54408
    };
    int i = 0;
    for (const auto& tt : tests) {
        auto rc = http::MaxBytesReader(nullptr, io::NopCloser(strings::NewReader(string(testStr))), tt.limit);
        std::vector<byte> p(tt.lenP);
        auto [n, err] = rc->Read(p);
        EXPECT_EQ(n, tt.wantN) << i;
        EXPECT_EQ(bool(err), tt.wantErr) << i << ". error: " << err;
        i++;
    }
}

// ---------------------------------------------------------------------------
// go++'s own

// A file uploaded through a server, as a browser posts a form: the handler
// gets its fields and file, and the server removes the temporary file.
TEST(NetHttpForm, UploadThroughServer) {
    auto tmpName = make_ptr<bytes::Buffer>();
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>([tmpName](http::ResponseWriter& w, http::Request& r) {
        if (auto err = r.ParseMultipartForm(16)) {  // the file goes to disk
            http::Error(w, err.Error(), http::StatusBadRequest);
            return;
        }
        auto [f, fh, err] = r.FormFile("upload");
        if (err) {
            http::Error(w, err.Error(), http::StatusBadRequest);
            return;
        }
        auto [data, _] = io::ReadAll(*f);
        f->Close();
        fmt::Print(w, "{} {} {} {}", r.FormValue("name"), fh->Filename, fh->Size, asStringView(data));
    }));
    auto body = make_ptr<bytes::Buffer>();
    auto mw = multipart::NewWriter(body);
    mw->WriteField("name", "gopher");
    auto [fw, err] = mw->CreateFormFile("upload", "notes.txt");
    ASSERT_FALSE(err);
    io::WriteString(*fw, "a file larger than sixteen bytes");
    ASSERT_FALSE(mw->Close());
    auto [res, perr] = ts->Client().Post(ts->URL, mw->FormDataContentType(), body);
    ASSERT_FALSE(perr) << perr;
    auto [got, rerr] = io::ReadAll(*res->Body);
    res->Body->Close();
    ASSERT_FALSE(rerr);
    EXPECT_EQ(res->StatusCode, http::StatusOK) << asStringView(got);
    EXPECT_EQ(asStringView(got), "gopher notes.txt 32 a file larger than sixteen bytes");
    ts->Close();
}

// PostForm sends url::Values as a urlencoded body.
TEST(NetHttpForm, PostForm) {
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>([](http::ResponseWriter& w, http::Request& r) {
        auto a = r.PostFormValue("a");  // parses the form
        fmt::Print(w, "{} {} {}", r.Header.Get("Content-Type"), a, r.Form["b"].size());
    }));
    url::Values v;
    v.Add("a", "1 2");
    v.Add("b", "x");
    v.Add("b", "y");
    auto [res, err] = ts->Client().PostForm(ts->URL + "?b=z", v);
    ASSERT_FALSE(err) << err;
    auto [got, _] = io::ReadAll(*res->Body);
    res->Body->Close();
    EXPECT_EQ(asStringView(got), "application/x-www-form-urlencoded 1 2 3");
    ts->Close();
}

// A body over a MaxBytesReader's limit: the handler gets a MaxBytesError,
// and the server closes the connection after the response.
TEST(NetHttpForm, MaxBytesReaderClosesConnection) {
    auto ts = httptest::NewServer(make_ptr<http::HandlerFunc>([](http::ResponseWriter& w, http::Request& r) {
        r.Body = http::MaxBytesReader(w, r.Body, 4);
        auto [_, err] = io::ReadAll(*r.Body);
        auto* mbe = errors::As<http::MaxBytesError>(err);
        fmt::Print(w, "{}", mbe ? mbe->Limit : -1);
    }));
    auto [res, err] = ts->Client().Post(ts->URL, "text/plain", strings::NewReader("too long a body"));
    ASSERT_FALSE(err) << err;
    auto [got, _] = io::ReadAll(*res->Body);
    res->Body->Close();
    EXPECT_EQ(asStringView(got), "4");
    EXPECT_TRUE(res->Close) << "the server keeps the connection";
    ts->Close();
}

}  // namespace
