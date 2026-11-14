// Tests ported from Go 1.27.3: mime/type_test.go, mime/type_unix_test.go and
// mime/mediatype_test.go. The tests of the tables use the package's
// internals, as Go's do (setMimeInit, setType, loadMimeFile).
//
// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in Go's LICENSE file.
#include "go_os_test_util.hpp"
#include "mime/internal.hpp"

#include <gopp/mime.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <map>

using namespace gopp;
namespace mi = gopp::mime::internal;
using os_test::TempDir;

namespace {

using smap = std::map<std::string, std::string>;

mime::Params m(std::initializer_list<std::pair<const char*, const char*>> kv) {
    mime::Params p;
    for (auto [k, v] : kv) p[string(std::string_view(k))] = string(std::string_view(v));
    return p;
}

std::string str(const string& s) { return std::string(s.view()); }

std::string paramsStr(const mime::Params& p) {
    std::string s = "{";
    for (const auto& [k, v] : p) s += str(k) + ":" + str(v) + " ";
    return s + "}";
}

// setMimeInit: the next lookup runs fn instead of the usual
// initialization; the cleanup restores it.
struct mimeInitGuard {
    explicit mimeInitGuard(std::function<void()> fn) { mi::set_mime_init(std::move(fn)); }
    ~mimeInitGuard() { mi::set_mime_init(nullptr); }
};

void clearMimeTypes() { mi::set_mime_types({}, {}); }

void set_type(std::string_view ext, std::string_view typ) {
    ASSERT_TRUE(ext.starts_with('.')) << "missing leading dot";
    if (auto err = mi::set_extension_type(ext, typ)) FAIL() << "bad test data: " << err;
}

void write_file(const std::string& name, std::string_view data) {
    ASSERT_FALSE(os::WriteFile(name, data, 0644));
}

constexpr std::string_view test_types =
    "# Copyright 2010 The Go Authors. All rights reserved.\n"
    "# Use of this source code is governed by a BSD-style\n"
    "# license that can be found in the LICENSE file.\n"
    "\n"
    "\n"
    " # mime package test\n"
    "application/test\tt1\t# Simple test\n"
    "text/test\t\tt2\t# Text test\n";

constexpr std::string_view test_types_globs2 =
    "# Copyright 2021 The Go Authors. All rights reserved.\n"
    "# Use of this source code is governed by a BSD-style\n"
    "# license that can be found in the LICENSE file.\n"
    "\n"
    "\n"
    "# mime package test for globs2\n"
    "50:document/test:*.t3\n"
    "50:example/test:*.t4\n"
    "50:text/plain:*,v\n"
    "50:application/x-trash:*~\n"
    "30:example/do-not-use:*.t4\n"
    "10:example/glob-question-mark:*.foo?ar\n"
    "10:example/glob-asterisk:*.foo*r\n"
    "10:example/glob-range:*.foo[1-3]\n";

// Points initialization at testdata files, and back at the system's.
struct systemFilesGuard {
    std::pair<std::vector<string>, std::vector<string>> old;
    systemFilesGuard(std::vector<string> globs, std::vector<string> types)
        : old(mi::setSystemFiles(std::move(globs), std::move(types))) {
        mi::set_mime_init(nullptr);
    }
    ~systemFilesGuard() {
        mi::setSystemFiles(std::move(old.first), std::move(old.second));
        mi::set_mime_init(nullptr);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// type_test.go

TEST(GoMime, TypeByExtension) {
    // initMimeForTests: no globs2 file, and testdata/test.types.
    TempDir dir;
    std::string types = os_test::join(dir, "test.types");
    write_file(types, test_types);
    systemFilesGuard files({string()}, {string(types)});

    smap tests = {
        {".T1", "application/test"},
        {".t2", "text/test; charset=utf-8"},
        {".png", "image/png"},
    };
    for (const auto& [ext, want] : tests) {
        EXPECT_EQ(mime::TypeByExtension(ext), want) << "TypeByExtension(" << ext << ")";
    }
}

TEST(GoMime, TypeByExtension_LocalData) {
    mimeInitGuard cleanup([] {
        clearMimeTypes();
        set_type(".foo", "x/foo");
        set_type(".bar", "x/bar");
        set_type(".Bar", "x/bar; capital=1");
    });
    smap tests = {
        {".foo", "x/foo"},
        {".bar", "x/bar"},
        {".Bar", "x/bar; capital=1"},
        {".sdlkfjskdlfj", ""},
        {".t1", ""},  // testdata shouldn't be used
    };
    for (const auto& [ext, want] : tests) {
        EXPECT_EQ(mime::TypeByExtension(ext), want) << "TypeByExtension(" << ext << ")";
    }
}

TEST(GoMime, TypeByExtensionCase) {
    constexpr std::string_view custom = "test/test; charset=iso-8859-1";
    constexpr std::string_view caps = "test/test; WAS=ALLCAPS";
    mimeInitGuard cleanup([&] {
        clearMimeTypes();
        set_type(".TEST", caps);
        set_type(".tesT", custom);
    });
    // case-sensitive lookup
    ASSERT_EQ(mime::TypeByExtension(".tesT"), custom);
    ASSERT_EQ(mime::TypeByExtension(".TEST"), caps);
    // case-insensitive
    ASSERT_EQ(mime::TypeByExtension(".TesT"), custom);
}

TEST(GoMime, AddExtensionType_TextMIMEWithParamsDefaultCharset) {
    mimeInitGuard cleanup([] { clearMimeTypes(); });
    constexpr std::string_view ext = ".regtxt";
    constexpr std::string_view in = "text/x-reg; myparam=myvalue";  // text/*, extra param, no charset
    ASSERT_FALSE(mime::AddExtensionType(ext, in));
    string got = mime::TypeByExtension(ext);
    ASSERT_FALSE(got.empty());
    ASSERT_TRUE(strings::Contains(got, "charset=utf-8")) << got;
    ASSERT_TRUE(got.starts_with("text/x-reg")) << got;
}

TEST(GoMime, AddExtensionTypeMissingDot) {
    auto err = mime::AddExtensionType("foo", "x/foo");
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), R"(mime: extension "foo" missing leading dot)");
}

TEST(GoMime, ExtensionsByType) {
    mimeInitGuard cleanup([] {
        clearMimeTypes();
        set_type(".gif", "image/gif");
        set_type(".a", "foo/letter");
        set_type(".b", "foo/letter");
        set_type(".B", "foo/letter");
        set_type(".PNG", "image/png");
    });
    struct {
        std::string typ;
        std::vector<std::string> want;
    } tests[] = {
        {"image/gif", {".gif"}},
        {"image/png", {".png"}},  // lowercase
        {"foo/letter", {".a", ".b"}},
        {"x/unknown", {}},
    };
    for (const auto& tt : tests) {
        auto [got, err] = mime::ExtensionsByType(tt.typ);
        ASSERT_FALSE(err) << tt.typ << ": " << err;
        std::vector<std::string> g;
        for (const auto& e : got) g.push_back(str(e));
        EXPECT_EQ(g, tt.want) << "ExtensionsByType(" << tt.typ << ")";
    }
}

TEST(GoMime, ExtensionsByType2) {
    mimeInitGuard cleanup([] {
        clearMimeTypes();
        // Initialize built-in types like in type.go before osInitMime.
        mi::set_mime_types(mi::builtin_types_lower(), mi::builtin_types_lower());
    });
    struct {
        std::string typ;
        std::vector<std::string> want;
    } tests[] = {
        {"application/postscript", {".ai", ".eps", ".ps"}},
        {"application/vnd.android.package-archive", {".apk"}},
        {"image/apng", {".apng"}},
        {"image/avif", {".avif"}},
        {"application/octet-stream", {".bin", ".com", ".exe"}},
        {"image/bmp", {".bmp"}},
        {"text/css; charset=utf-8", {".css"}},
        {"text/csv; charset=utf-8", {".csv"}},
        {"application/msword", {".doc"}},
        {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", {".docx"}},
        {"text/html; charset=utf-8", {".ehtml", ".htm", ".html", ".shtml"}},
        {"message/rfc822", {".eml"}},
        {"audio/flac", {".flac"}},
        {"image/gif", {".gif"}},
        {"application/gzip", {".gz"}},
        {"image/vnd.microsoft.icon", {".ico"}},
        {"text/calendar; charset=utf-8", {".ics"}},
        {"image/jpeg", {".jfif", ".jpeg", ".jpg", ".pjp", ".pjpeg"}},
        {"text/javascript; charset=utf-8", {".js", ".mjs"}},
        {"application/json", {".json"}},
        {"audio/mp4", {".m4a"}},
        {"audio/mpeg", {".mp3"}},
        {"video/mp4", {".mp4"}},
        {"audio/ogg", {".oga", ".ogg", ".opus"}},
        {"video/ogg", {".ogv"}},
        {"application/pdf", {".pdf"}},
        {"image/png", {".png"}},
        {"application/vnd.ms-powerpoint", {".ppt"}},
        {"application/vnd.openxmlformats-officedocument.presentationml.presentation", {".pptx"}},
        {"application/rdf+xml", {".rdf"}},
        {"application/rtf", {".rtf"}},
        {"image/svg+xml", {".svg"}},
        {"text/plain; charset=utf-8", {".text", ".txt"}},
        {"image/tiff", {".tif", ".tiff"}},
        {"text/vtt; charset=utf-8", {".vtt"}},
        {"application/wasm", {".wasm"}},
        {"audio/wav", {".wav"}},
        {"audio/webm", {".weba"}},
        {"video/webm", {".webm"}},
        {"image/webp", {".webp"}},
        {"text/xml; charset=utf-8", {".xbl", ".xml", ".xsl"}},
        {"image/x-xbitmap", {".xbm"}},
        {"application/xhtml+xml", {".xht", ".xhtml"}},
        {"application/vnd.ms-excel", {".xls"}},
        {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", {".xlsx"}},
        {"application/zip", {".zip"}},
    };
    for (const auto& tt : tests) {
        auto [got, err] = mime::ExtensionsByType(tt.typ);
        ASSERT_FALSE(err) << tt.typ << ": " << err;
        std::vector<std::string> g;
        for (const auto& e : got) g.push_back(str(e));
        EXPECT_EQ(g, tt.want) << "ExtensionsByType(" << tt.typ << ")";
    }
}

// ---------------------------------------------------------------------------
// type_unix_test.go

TEST(GoMime, TypeByExtensionUNIX) {
    TempDir dir;
    std::string globs = os_test::join(dir, "test.types.globs2");
    std::string types = os_test::join(dir, "test.types");
    write_file(globs, test_types_globs2);
    write_file(types, test_types);
    // initMimeUnixTest: the usual initialization, then the testdata files.
    systemFilesGuard files({string()}, {});
    mi::ensureInit();
    ASSERT_FALSE(mi::loadMimeGlobsFile(globs));
    mi::load_mime_file(types);

    smap tests = {
        {".T1", "application/test"},
        {".t2", "text/test; charset=utf-8"},
        {".t3", "document/test"},
        {".t4", "example/test"},
        {".png", "image/png"},
        {",v", ""},
        {"~", ""},
        {".foo?ar", ""},
        {".foo*r", ""},
        {".foo[1-3]", ""},
    };
    for (const auto& [ext, want] : tests) {
        EXPECT_EQ(mime::TypeByExtension(ext), want) << "TypeByExtension(" << ext << ")";
    }
}

// The system's tables: the built-in types win over globs2, so ".css" keeps
// its charset.
TEST(GoMime, TypeByExtensionSystem) {
    EXPECT_EQ(mime::TypeByExtension(".css"), "text/css; charset=utf-8");
    EXPECT_EQ(mime::TypeByExtension(".HTML"), "text/html; charset=utf-8");
    EXPECT_EQ(mime::TypeByExtension(".png"), "image/png");
    EXPECT_EQ(mime::TypeByExtension(""), "");
}

// ---------------------------------------------------------------------------
// mediatype_test.go

TEST(GoMime, ConsumeToken) {
    struct {
        std::string v, token, rest;
    } tests[] = {
        {"foo bar", "foo", " bar"},
        {"bar", "bar", ""},
        {"", "", ""},
        {" foo", "", " foo"},
    };
    for (const auto& test : tests) {
        auto [token, rest] = mi::consume_token(test.v);
        EXPECT_EQ(token, test.token) << test.v;
        EXPECT_EQ(rest, test.rest) << test.v;
    }
}

TEST(GoMime, ConsumeValue) {
    struct {
        std::string v, value, rest;
    } tests[] = {
        {"foo bar", "foo", " bar"},
        {"bar", "bar", ""},
        {" bar ", "", " bar "},
        {R"("My value"end)", "My value", "end"},
        {R"("My value" end)", "My value", " end"},
        {R"("\\" rest)", "\\", " rest"},
        {R"("My \" value"end)", "My \" value", "end"},
        {R"("\" rest)", "", R"("\" rest)"},
        {"\"C:\\dev\\go\\robots.txt\"", "C:\\dev\\go\\robots.txt", ""},
        {"\"C:\\新建文件夹\\中文第二次测试.mp4\"", "C:\\新建文件夹\\中文第二次测试.mp4", ""},
    };
    for (const auto& test : tests) {
        auto [value, rest] = mi::consume_value(test.v);
        EXPECT_EQ(value, test.value) << test.v;
        EXPECT_EQ(rest, test.rest) << test.v;
    }
}

TEST(GoMime, ConsumeMediaParam) {
    struct {
        std::string v, attribute, value, rest;
    } tests[] = {
        {" ; foo=bar", "foo", "bar", ""},
        {"; foo=bar", "foo", "bar", ""},
        {";foo=bar", "foo", "bar", ""},
        {";FOO=bar", "foo", "bar", ""},
        {R"(;foo="bar")", "foo", "bar", ""},
        {R"(;foo="bar"; )", "foo", "bar", "; "},
        {R"(;foo="bar"; foo=baz)", "foo", "bar", "; foo=baz"},
        {R"( ; boundary=----CUT;)", "boundary", "----CUT", ";"},
        {R"( ; key=value;  blah="value";name="foo" )", "key", "value", R"(;  blah="value";name="foo" )"},
        {R"(;  blah="value";name="foo" )", "blah", "value", R"(;name="foo" )"},
        {R"(;name="foo" )", "name", "foo", " "},
        // go++: no value at the end of the input, as in Go's
        // TestParseFormUnknownContentType of net/http.
        {R"(; boundary=)", "", "", R"(; boundary=)"},
    };
    for (const auto& test : tests) {
        auto [attribute, value, rest] = mi::consume_media_param(test.v);
        EXPECT_EQ(attribute, test.attribute) << test.v;
        EXPECT_EQ(value, test.value) << test.v;
        EXPECT_EQ(rest, test.rest) << test.v;
    }
}

namespace {

struct media_type_test {
    std::string in;
    std::string t;
    mime::Params p;
};

std::vector<media_type_test> parseMediaTypeTests() {
    auto name_foo = m({{"name", "foo"}});
    return {
        {R"(form-data; name="foo")", "form-data", name_foo},
        {R"( form-data ; name=foo)", "form-data", name_foo},
        {R"(FORM-DATA;name="foo")", "form-data", name_foo},
        {R"( FORM-DATA ; name="foo")", "form-data", name_foo},
        {R"( FORM-DATA ; name="foo")", "form-data", name_foo},

        {R"(form-data; key=value;  blah="value";name="foo" )", "form-data",
         m({{"key", "value"}, {"blah", "value"}, {"name", "foo"}})},

        {R"(foo; key=val1; key=the-key-appears-again-which-is-bogus)", "", m({})},

        // From RFC 2231:
        {R"(application/x-stuff; title*=us-ascii'en-us'This%20is%20%2A%2A%2Afun%2A%2A%2A)", "application/x-stuff",
         m({{"title", "This is ***fun***"}})},

        {R"(message/external-body; access-type=URL; )"
         R"(URL*0="ftp://";)"
         R"(URL*1="cs.utk.edu/pub/moore/bulk-mailer/bulk-mailer.tar")",
         "message/external-body",
         m({{"access-type", "URL"}, {"url", "ftp://cs.utk.edu/pub/moore/bulk-mailer/bulk-mailer.tar"}})},

        {R"(application/x-stuff; )"
         R"(title*0*=us-ascii'en'This%20is%20even%20more%20; )"
         R"(title*1*=%2A%2A%2Afun%2A%2A%2A%20; )"
         R"(title*2="isn't it!")",
         "application/x-stuff", m({{"title", "This is even more ***fun*** isn't it!"}})},

        // Tests from http://greenbytes.de/tech/tc2231/
        // Note: Backslash escape handling is a bit loose, like MSIE.

        // #attonly
        {R"(attachment)", "attachment", m({})},
        // #attonlyucase
        {R"(ATTACHMENT)", "attachment", m({})},
        // #attwithasciifilename
        {R"(attachment; filename="foo.html")", "attachment", m({{"filename", "foo.html"}})},
        // #attwithasciifilename25
        {R"(attachment; filename="0000000000111111111122222")", "attachment",
         m({{"filename", "0000000000111111111122222"}})},
        // #attwithasciifilename35
        {R"(attachment; filename="00000000001111111111222222222233333")", "attachment",
         m({{"filename", "00000000001111111111222222222233333"}})},
        // #attwithasciifnescapedchar
        {R"(attachment; filename="f\oo.html")", "attachment", m({{"filename", "f\\oo.html"}})},
        // #attwithasciifnescapedquote
        {R"(attachment; filename="\"quoting\" tested.html")", "attachment",
         m({{"filename", R"("quoting" tested.html)"}})},
        // #attwithquotedsemicolon
        {R"(attachment; filename="Here's a semicolon;.html")", "attachment",
         m({{"filename", "Here's a semicolon;.html"}})},
        // #attwithfilenameandextparam
        {R"(attachment; foo="bar"; filename="foo.html")", "attachment",
         m({{"foo", "bar"}, {"filename", "foo.html"}})},
        // #attwithfilenameandextparamescaped
        {R"(attachment; foo="\"\\";filename="foo.html")", "attachment",
         m({{"foo", "\"\\"}, {"filename", "foo.html"}})},
        // #attwithasciifilenameucase
        {R"(attachment; FILENAME="foo.html")", "attachment", m({{"filename", "foo.html"}})},
        // #attwithasciifilenamenq
        {R"(attachment; filename=foo.html)", "attachment", m({{"filename", "foo.html"}})},
        // #attwithasciifilenamenqs
        {R"(attachment; filename=foo.html ;)", "attachment", m({{"filename", "foo.html"}})},
        // #attwithfntokensq
        {R"(attachment; filename='foo.html')", "attachment", m({{"filename", "'foo.html'"}})},
        // #attwithisofnplain
        {R"(attachment; filename="foo-ä.html")", "attachment", m({{"filename", "foo-ä.html"}})},
        // #attwithutf8fnplain
        {R"(attachment; filename="foo-Ã¤.html")", "attachment", m({{"filename", "foo-Ã¤.html"}})},
        // #attwithfnrawpctenca
        {R"(attachment; filename="foo-%41.html")", "attachment", m({{"filename", "foo-%41.html"}})},
        // #attwithfnusingpct
        {R"(attachment; filename="50%.html")", "attachment", m({{"filename", "50%.html"}})},
        // #attwithfnrawpctencaq
        {R"(attachment; filename="foo-%\41.html")", "attachment", m({{"filename", "foo-%\\41.html"}})},
        // #attwithnamepct
        {R"(attachment; name="foo-%41.html")", "attachment", m({{"name", "foo-%41.html"}})},
        // #attwithfilenamepctandiso
        {R"(attachment; name="ä-%41.html")", "attachment", m({{"name", "ä-%41.html"}})},
        // #attwithfnrawpctenclong
        {R"(attachment; filename="foo-%c3%a4-%e2%82%ac.html")", "attachment",
         m({{"filename", "foo-%c3%a4-%e2%82%ac.html"}})},
        // #attwithasciifilenamews1
        {R"(attachment; filename ="foo.html")", "attachment", m({{"filename", "foo.html"}})},
        // #attmissingdisposition
        {R"(filename=foo.html)", "", m({})},
        // #attmissingdisposition2
        {R"(x=y; filename=foo.html)", "", m({})},
        // #attmissingdisposition3
        {R"("foo; filename=bar;baz"; filename=qux)", "", m({})},
        // #attmissingdisposition4
        {R"(filename=foo.html, filename=bar.html)", "", m({})},
        // #emptydisposition
        {R"(; filename=foo.html)", "", m({})},
        // #doublecolon
        {R"(: inline; attachment; filename=foo.html)", "", m({})},
        // #attandinline
        {R"(inline; attachment; filename=foo.html)", "", m({})},
        // #attandinline2
        {R"(attachment; inline; filename=foo.html)", "", m({})},
        // #attbrokenquotedfn
        {R"(attachment; filename="foo.html".txt)", "", m({})},
        // #attbrokenquotedfn2
        {R"(attachment; filename="bar)", "", m({})},
        // #attbrokenquotedfn3
        {R"(attachment; filename=foo"bar;baz"qux)", "", m({})},
        // #attmultinstances
        {R"(attachment; filename=foo.html, attachment; filename=bar.html)", "", m({})},
        // #attmissingdelim
        {R"(attachment; foo=foo filename=bar)", "", m({})},
        // #attmissingdelim2
        {R"(attachment; filename=bar foo=foo)", "", m({})},
        // #attmissingdelim3
        {R"(attachment filename=bar)", "", m({})},
        // #attreversed
        {R"(filename=foo.html; attachment)", "", m({})},
        // #attconfusedparam
        {R"(attachment; xfilename=foo.html)", "attachment", m({{"xfilename", "foo.html"}})},
        // #attcdate
        {R"(attachment; creation-date="Wed, 12 Feb 1997 16:29:51 -0500")", "attachment",
         m({{"creation-date", "Wed, 12 Feb 1997 16:29:51 -0500"}})},
        // #attmdate
        {R"(attachment; modification-date="Wed, 12 Feb 1997 16:29:51 -0500")", "attachment",
         m({{"modification-date", "Wed, 12 Feb 1997 16:29:51 -0500"}})},
        // #dispext
        {R"(foobar)", "foobar", m({})},
        // #dispextbadfn
        {R"(attachment; example="filename=example.txt")", "attachment",
         m({{"example", "filename=example.txt"}})},
        // #attwithfn2231utf8
        {R"(attachment; filename*=UTF-8''foo-%c3%a4-%e2%82%ac.html)", "attachment",
         m({{"filename", "foo-ä-€.html"}})},
        // #attwithfn2231noc
        {R"(attachment; filename*=''foo-%c3%a4-%e2%82%ac.html)", "attachment", m({})},
        // #attwithfn2231utf8comp
        {R"(attachment; filename*=UTF-8''foo-a%cc%88.html)", "attachment", m({{"filename", "foo-a\xcc\x88.html"}})},
        // #attwithfn2231ws2
        {R"(attachment; filename*= UTF-8''foo-%c3%a4.html)", "attachment", m({{"filename", "foo-ä.html"}})},
        // #attwithfn2231ws3
        {R"(attachment; filename* =UTF-8''foo-%c3%a4.html)", "attachment", m({{"filename", "foo-ä.html"}})},
        // #attwithfn2231quot
        {R"(attachment; filename*="UTF-8''foo-%c3%a4.html")", "attachment", m({{"filename", "foo-ä.html"}})},
        // #attwithfn2231quot2
        {R"(attachment; filename*="foo%20bar.html")", "attachment", m({})},
        // #attwithfn2231singleqmissing
        {R"(attachment; filename*=UTF-8'foo-%c3%a4.html)", "attachment", m({})},
        // #attwithfn2231nbadpct1
        {R"(attachment; filename*=UTF-8''foo%)", "attachment", m({})},
        // #attwithfn2231nbadpct2
        {R"(attachment; filename*=UTF-8''f%oo.html)", "attachment", m({})},
        // #attwithfn2231dpct
        {R"(attachment; filename*=UTF-8''A-%2541.html)", "attachment", m({{"filename", "A-%41.html"}})},
        // #attfncont
        {R"(attachment; filename*0="foo."; filename*1="html")", "attachment", m({{"filename", "foo.html"}})},
        // #attfncontenc
        {R"(attachment; filename*0*=UTF-8''foo-%c3%a4; filename*1=".html")", "attachment",
         m({{"filename", "foo-ä.html"}})},
        // #attfncontlz
        {R"(attachment; filename*0="foo"; filename*01="bar")", "attachment", m({{"filename", "foo"}})},
        // #attfncontnc
        {R"(attachment; filename*0="foo"; filename*2="bar")", "attachment", m({{"filename", "foo"}})},
        // #attfnconts1
        {R"(attachment; filename*1="foo."; filename*2="html")", "attachment", m({})},
        // #attfncontord
        {R"(attachment; filename*1="bar"; filename*0="foo")", "attachment", m({{"filename", "foobar"}})},
        // #attfnboth
        {R"(attachment; filename="foo-ae.html"; filename*=UTF-8''foo-%c3%a4.html)", "attachment",
         m({{"filename", "foo-ä.html"}})},
        // #attfnboth2
        {R"(attachment; filename*=UTF-8''foo-%c3%a4.html; filename="foo-ae.html")", "attachment",
         m({{"filename", "foo-ä.html"}})},
        // #attfnboth3
        {R"(attachment; filename*0*=ISO-8859-15''euro-sign%3d%a4; filename*=ISO-8859-1''currency-sign%3d%a4)",
         "attachment", m({})},
        // #attnewandfn
        {R"(attachment; foobar=x; filename="foo.html")", "attachment",
         m({{"foobar", "x"}, {"filename", "foo.html"}})},

        // Browsers also just send UTF-8 directly without RFC 2231,
        // at least when the source page is served with UTF-8.
        {R"(form-data; firstname="Брэд"; lastname="Фицпатрик")", "form-data",
         m({{"firstname", "Брэд"}, {"lastname", "Фицпатрик"}})},

        // Empty string used to be mishandled.
        {R"(foo; bar="")", "foo", m({{"bar", ""}})},

        // Microsoft browsers in intranet mode do not think they need to escape \ in file name.
        {R"(form-data; name="file"; filename="C:\dev\go\robots.txt")", "form-data",
         m({{"name", "file"}, {"filename", R"(C:\dev\go\robots.txt)"}})},
        {R"(form-data; name="file"; filename="C:\新建文件夹\中文第二次测试.mp4")", "form-data",
         m({{"name", "file"}, {"filename", R"(C:\新建文件夹\中文第二次测试.mp4)"}})},

        // issue #46323 (https://github.com/golang/go/issues/46323)
        {
            // example from rfc2231-p.3 (https://datatracker.ietf.org/doc/html/rfc2231)
            "message/external-body; access-type=URL;\n"
            "\t\tURL*0=\"ftp://\";\n"
            "\t\tURL*1=\"cs.utk.edu/pub/moore/bulk-mailer/bulk-mailer.tar\";",  // <-- trailing semicolon
            "message/external-body",
            m({{"access-type", "URL"}, {"url", "ftp://cs.utk.edu/pub/moore/bulk-mailer/bulk-mailer.tar"}}),
        },

        // Issue #48866: duplicate parameters containing equal values should be allowed
        {R"(text; charset=utf-8; charset=utf-8; format=fixed)", "text",
         m({{"charset", "utf-8"}, {"format", "fixed"}})},
        {R"(text; charset=utf-8; format=flowed; charset=utf-8)", "text",
         m({{"charset", "utf-8"}, {"format", "flowed"}})},

        // Issue #76236: '{' and '}' are token chars.
        {"attachment; filename={file}.png", "attachment", m({{"filename", "{file}.png"}})},
    };
}

}  // namespace

TEST(GoMime, ParseMediaType) {
    for (const auto& test : parseMediaTypeTests()) {
        auto [mt, params, err] = mime::ParseMediaType(test.in);
        if (err) {
            EXPECT_EQ(test.t, "") << "for input " << test.in << ", unexpected error: " << err;
            continue;
        }
        EXPECT_EQ(mt, test.t) << "for input " << test.in;
        if (mt != test.t) continue;
        if (params.empty() && test.p.empty()) continue;
        EXPECT_EQ(params, test.p) << "for input " << test.in << ", wrong params: expected " << paramsStr(test.p)
                                  << ", got " << paramsStr(params);
    }
}

TEST(GoMime, ParseMediaTypeBogus) {
    struct {
        std::string in, mt, err;
    } tests[] = {
        {"bogus ;=========", "bogus", "mime: invalid media parameter"},
        // The following example is from real email delivered by gmail (error: missing semicolon)
        // and it is there to check behavior described in #19498
        {R"(application/pdf; x-mac-type="3F3F3F3F"; x-mac-creator="3F3F3F3F" name="a.pdf";)", "application/pdf",
         "mime: invalid media parameter"},
        {"bogus/<script>alert</script>", "", "mime: expected token after slash"},
        {"bogus/bogus<script>alert</script>", "", "mime: unexpected content after media subtype"},
        // Tests from http://greenbytes.de/tech/tc2231/
        {R"("attachment")", "attachment", "mime: no media type"},
        {"attachment; filename=foo,bar.html", "attachment", "mime: invalid media parameter"},
        {"attachment; ;filename=foo", "attachment", "mime: invalid media parameter"},
        {"attachment; filename=foo bar.html", "attachment", "mime: invalid media parameter"},
        {R"(attachment; filename="foo.html"; filename="bar.html")", "attachment", "mime: duplicate parameter name"},
        {"attachment; filename=foo[1](2).html", "attachment", "mime: invalid media parameter"},
        {"attachment; filename=foo-ä.html", "attachment", "mime: invalid media parameter"},
        {"attachment; filename=foo-Ã¤.html", "attachment", "mime: invalid media parameter"},
        {R"(attachment; filename *=UTF-8''foo-%c3%a4.html)", "attachment", "mime: invalid media parameter"},
    };
    for (const auto& tt : tests) {
        auto [mt, params, err] = mime::ParseMediaType(tt.in);
        if (!err) {
            ADD_FAILURE() << "ParseMediaType(" << tt.in << ") = nil error; want parse error";
            continue;
        }
        EXPECT_EQ(err.Error(), tt.err) << tt.in;
        EXPECT_TRUE(params.empty()) << "ParseMediaType(" << tt.in << "): got params on error";
        if (err != mime::ErrInvalidMediaParameter) {
            EXPECT_EQ(mt, "") << "ParseMediaType(" << tt.in << "): got unexpected non-empty media type string";
        } else {
            EXPECT_EQ(mt, tt.mt) << "ParseMediaType(" << tt.in << "): in case of invalid parameters";
        }
    }
}

TEST(GoMime, FormatMediaType) {
    struct {
        std::string typ;
        mime::Params params;
        std::string want;
    } tests[] = {
        {"noslash", m({{"X", "Y"}}), "noslash; x=Y"},  // e.g. Content-Disposition values (RFC 2183); issue 11289
        {"foo bar/baz", {}, ""},
        {"foo/bar baz", {}, ""},
        {"attachment", m({{"filename", "ĄĄŽŽČČŠŠ"}}),
         "attachment; filename*=utf-8''%C4%84%C4%84%C5%BD%C5%BD%C4%8C%C4%8C%C5%A0%C5%A0"},
        {"attachment", m({{"filename", "ÁÁÊÊÇÇÎÎ"}}),
         "attachment; filename*=utf-8''%C3%81%C3%81%C3%8A%C3%8A%C3%87%C3%87%C3%8E%C3%8E"},
        {"attachment", m({{"filename", "数据统计.png"}}),
         "attachment; filename*=utf-8''%E6%95%B0%E6%8D%AE%E7%BB%9F%E8%AE%A1.png"},
        {"foo/BAR", {}, "foo/bar"},
        {"foo/BAR", m({{"X", "Y"}}), "foo/bar; x=Y"},
        {"foo/BAR", m({{"space", "With space"}}), R"(foo/bar; space="With space")"},
        {"foo/BAR", m({{"quote", R"(With "quote)"}}), R"(foo/bar; quote="With \"quote")"},
        {"foo/BAR", m({{"bslash", R"(With \backslash)"}}), R"(foo/bar; bslash="With \\backslash")"},
        {"foo/BAR", m({{"both", R"(With \backslash and "quote)"}}),
         R"(foo/bar; both="With \\backslash and \"quote")"},
        {"foo/BAR", m({{"", "empty attribute"}}), ""},
        {"foo/BAR", m({{"bad attribute", "baz"}}), ""},
        {"foo/BAR", m({{"nonascii", "not an ascii character: ä"}}),
         "foo/bar; nonascii*=utf-8''not%20an%20ascii%20character%3A%20%C3%A4"},
        {"foo/BAR", {{string::fromStatic("ctl"), string(std::string_view("newline: \n nil: \0", 17))}},
         "foo/bar; ctl*=utf-8''newline%3A%20%0A%20nil%3A%20%00"},
        {"foo/bar", m({{"a", "av"}, {"b", "bv"}, {"c", "cv"}}), "foo/bar; a=av; b=bv; c=cv"},
        {"foo/bar", m({{"0", "'"}, {"9", "'"}}), "foo/bar; 0='; 9='"},
        {"foo", m({{"bar", ""}}), R"(foo; bar="")"},
    };
    int i = 0;
    for (const auto& tt : tests) {
        string got = mime::FormatMediaType(tt.typ, tt.params);
        EXPECT_EQ(got, tt.want) << i << ". FormatMediaType(" << tt.typ << ", " << paramsStr(tt.params) << ")";
        i++;
        if (got.empty()) continue;
        auto [typ, params, err] = mime::ParseMediaType(got);
        EXPECT_FALSE(err) << "ParseMediaType(" << got << "): " << err;
        EXPECT_EQ(typ, strings::ToLower(tt.typ)) << "ParseMediaType(" << got << ")";
        for (const auto& [k, v] : tt.params) {
            EXPECT_EQ(params[strings::ToLower(k)], v) << "ParseMediaType(" << got << ") params[" << k << "]";
        }
    }
}
