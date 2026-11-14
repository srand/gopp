// Ported from Go 1.27.3's mime/quotedprintable/reader_test.go and
// writer_test.go, with Go's test names. Left out: TestExhaustive, which
// compares with the qprint program, and the benchmarks.
#include <gopp/bytes.hpp>
#include <gopp/io.hpp>
#include <gopp/mime/quotedprintable.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string repeat(std::string_view s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

TEST(GoQuotedprintable, Reader) {
    struct test {
        std::string in, want, err;
    };
    std::vector<test> tests = {
        {"", "", ""},
        {"foo bar", "foo bar", ""},
        {"foo bar=3D", "foo bar=", ""},
        {"foo bar=3d", "foo bar=", ""},  // lax.
        {"foo bar=\n", "foo bar", ""},
        {"foo bar\n", "foo bar\n", ""},  // somewhat lax.
        {"foo bar=0", "foo bar=0", ""},  // lax
        {"foo bar=0D=0A", "foo bar\r\n", ""},
        {" A B        \r\n C ", " A B\r\n C", ""},
        {" A B =\r\n C ", " A B  C", ""},
        {" A B =\n C ", " A B  C", ""},  // lax. treating LF as CRLF
        {"foo=\nbar", "foobar", ""},
        {std::string("foo\x00" "bar", 7), "foo", "quotedprintable: invalid unescaped byte 0x00 in body"},
        {"foo bar\xff", "foo bar\xff", ""},

        // Equal sign.
        {"=3D30\n", "=30\n", ""},
        {"=00=FF0=\n", std::string("\x00\xff" "0", 3), ""},

        // Trailing whitespace
        {"foo  \n", "foo\n", ""},
        {"foo  \n\nfoo =\n\nfoo=20\n\n", "foo\n\nfoo \nfoo \n\n", ""},

        // Tests that we allow bare \n and \r through, despite it being
        // strictly not permitted per RFC 2045, Section 6.7 Page 22 bullet
        // (4).
        {"foo\nbar", "foo\nbar", ""},
        {"foo\rbar", "foo\rbar", ""},
        {"foo\r\nbar", "foo\r\nbar", ""},

        // Different types of soft line-breaks.
        {"foo=\r\nbar", "foobar", ""},
        {"foo=\nbar", "foobar", ""},
        {"foo=\rbar", "foo", "quotedprintable: invalid hex byte 0x0d"},
        {"foo=\r\r\r \nbar", "foo", R"(quotedprintable: invalid bytes after =: "\r\r\r \n")"},
        // Issue 15486, accept trailing soft line-break at end of input.
        {"foo=", "foo", ""},
        {"=", "", R"(quotedprintable: invalid bytes after =: "")"},

        // Example from RFC 2045:
        {"Now's the time =\nfor all folk to come=\n to the aid of their country.",
         "Now's the time for all folk to come to the aid of their country.", ""},
        {"accept UTF-8 right quotation mark: ’", "accept UTF-8 right quotation mark: ’", ""},

        // Transport padding.
        {"foo= \r\nbar", "foobar", ""},
        {"foo=\t \r\nbar", "foobar", ""},
    };
    for (const auto& tt : tests) {
        strings::Builder buf;
        auto [_, err] = io::Copy(buf, *quotedprintable::NewReader(strings::NewReader(string(tt.in))));
        EXPECT_EQ(buf.String(), tt.want) << "for " << strconv::Quote(tt.in);
        if (tt.err.empty()) {
            EXPECT_FALSE(err) << "for " << strconv::Quote(tt.in) << ", got unexpected error: " << err;
        } else {
            EXPECT_EQ(err.Error(), tt.err) << "for " << strconv::Quote(tt.in);
        }
    }
}

void testWriter(bool binary) {
    struct test {
        std::string in, want, wantB;
    };
    std::vector<test> tests = {
        {"", "", ""},
        {"foo bar", "foo bar", ""},
        {"foo bar=", "foo bar=3D", ""},
        {"foo bar\r", "foo bar\r\n", "foo bar=0D"},
        {"foo bar\r\r", "foo bar\r\n\r\n", "foo bar=0D=0D"},
        {"foo bar\n", "foo bar\r\n", "foo bar=0A"},
        {"foo bar\r\n", "foo bar\r\n", "foo bar=0D=0A"},
        {"foo bar\r\r\n", "foo bar\r\n\r\n", "foo bar=0D=0D=0A"},
        {"foo bar ", "foo bar=20", ""},
        {"foo bar\t", "foo bar=09", ""},
        {"foo bar  ", "foo bar =20", ""},
        {"foo bar \n", "foo bar=20\r\n", "foo bar =0A"},
        {"foo bar \r", "foo bar=20\r\n", "foo bar =0D"},
        {"foo bar \r\n", "foo bar=20\r\n", "foo bar =0D=0A"},
        {"foo bar  \n", "foo bar =20\r\n", "foo bar  =0A"},
        {"foo bar  \n ", "foo bar =20\r\n=20", "foo bar  =0A=20"},
        {"¡Hola Señor!", "=C2=A1Hola Se=C3=B1or!", ""},
        {"\t !\"#$%&'()*+,-./ :;<>?@[\\]^_`{|}~", "\t !\"#$%&'()*+,-./ :;<>?@[\\]^_`{|}~", ""},
        {repeat("a", 75), repeat("a", 75), ""},
        {repeat("a", 76), repeat("a", 75) + "=\r\na", ""},
        {repeat("a", 72) + "=", repeat("a", 72) + "=3D", ""},
        {repeat("a", 73) + "=", repeat("a", 73) + "=\r\n=3D", ""},
        {repeat("a", 74) + "=", repeat("a", 74) + "=\r\n=3D", ""},
        {repeat("a", 75) + "=", repeat("a", 75) + "=\r\n=3D", ""},
        {repeat(" ", 73), repeat(" ", 72) + "=20", ""},
        {repeat(" ", 74), repeat(" ", 73) + "=\r\n=20", ""},
        {repeat(" ", 75), repeat(" ", 74) + "=\r\n=20", ""},
        {repeat(" ", 76), repeat(" ", 75) + "=\r\n=20", ""},
        {repeat(" ", 77), repeat(" ", 75) + "=\r\n =20", ""},
    };
    for (const auto& tt : tests) {
        auto buf = make_ptr<strings::Builder>();
        auto w = quotedprintable::NewWriter(buf);
        std::string want = tt.want;
        if (binary) {
            w->Binary = true;
            if (!tt.wantB.empty()) want = tt.wantB;
        }
        if (auto [_, err] = io::WriteString(*w, tt.in); err) {
            ADD_FAILURE() << "Write(" << strconv::Quote(tt.in) << "): " << err;
            continue;
        }
        if (auto err = w->Close()) {
            ADD_FAILURE() << "Close(): " << err;
            continue;
        }
        EXPECT_EQ(buf->String(), want) << "Write(" << strconv::Quote(tt.in) << ")";
    }
}

TEST(GoQuotedprintable, Writer) { testWriter(false); }
TEST(GoQuotedprintable, WriterBinary) { testWriter(true); }

const std::string testMsg =
    "Quoted-Printable (QP) est un format d'encodage de données codées sur 8 bits, qui utilise exclusivement les "
    "caractères alphanumériques imprimables du code ASCII (7 bits).\r\n"
    "\r\n"
    "En effet, les différents codages comprennent de nombreux caractères qui ne sont pas représentables en ASCII "
    "(par exemple les caractères accentués), ainsi que des caractères dits « non-imprimables ».\r\n"
    "\r\n"
    "L'encodage Quoted-Printable permet de remédier à ce problème, en procédant de la manière suivante :\r\n"
    "\r\n"
    "Un octet correspondant à un caractère imprimable de l'ASCII sauf le signe égal (donc un caractère de code "
    "ASCII entre 33 et 60 ou entre 62 et 126) ou aux caractères de saut de ligne (codes ASCII 13 et 10) ou une suite "
    "de tabulations et espaces non situées en fin de ligne (de codes ASCII respectifs 9 et 32) est représenté tel "
    "quel.\r\n"
    "Un octet qui ne correspond pas à la définition ci-dessus (caractère non imprimable de l'ASCII, tabulation ou "
    "espaces non suivies d'un caractère imprimable avant la fin de la ligne ou signe égal) est représenté par un signe "
    "égal, suivi de son numéro, exprimé en hexadécimal.\r\n"
    "Enfin, un signe égal suivi par un saut de ligne (donc la suite des trois caractères de codes ASCII 61, 13 et 10) "
    "peut être inséré n'importe où, afin de limiter la taille des lignes produites si nécessaire. Une limite de 76 "
    "caractères par ligne est généralement respectée.\r\n";

TEST(GoQuotedprintable, RoundTrip) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = quotedprintable::NewWriter(buf);
    ASSERT_FALSE(io::WriteString(*w, testMsg).second);
    ASSERT_FALSE(w->Close());
    auto r = quotedprintable::NewReader(buf);
    auto [gotBytes, err] = io::ReadAll(*r);
    ASSERT_FALSE(err) << "Error while reading from Reader: " << err;
    EXPECT_EQ(std::string(asStringView(gotBytes)), testMsg);
}

}  // namespace
