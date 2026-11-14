// Ported from Go 1.27.3's regexp/exec_test.go, with Go's test names:
// TestRE2Search runs RE2's search tests (testdata/re2-search.txt) in the
// four modes (full and partial, first and longest match), TestFowler the
// AT&T POSIX tests (testdata/*.dat), embedded in go_regexp_testdata.hpp.
// Left out: TestRE2Exhaustive (a 400 KB bzip2 file), the benchmarks.
#include "go_regexp_testdata.hpp"

#include <gopp/regexp.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace gopp;

namespace {

// strconv.Unquote for the double-quoted strings of the test files.
std::optional<std::string> unquote(std::string_view s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return std::nullopt;
    s = s.substr(1, s.size() - 2);
    std::string out;
    auto hex = [](std::string_view h) -> std::optional<uint32> {
        uint32 v = 0;
        for (char c : h) {
            v <<= 4;
            if ('0' <= c && c <= '9') v |= uint32(c - '0');
            else if ('a' <= c && c <= 'f') v |= uint32(c - 'a' + 10);
            else if ('A' <= c && c <= 'F') v |= uint32(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    };
    auto putRune = [&](rune r) {
        char buf[4];
        auto n = utf8::EncodeRune(std::span<byte>(reinterpret_cast<byte*>(buf), 4), r);
        out.append(buf, std::size_t(n));
    };
    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '"') return std::nullopt;
        if (c != '\\') {
            out += c;
            i++;
            continue;
        }
        if (i + 1 >= s.size()) return std::nullopt;
        char e = s[i + 1];
        switch (e) {
        case 'a': out += '\a'; i += 2; break;
        case 'b': out += '\b'; i += 2; break;
        case 'f': out += '\f'; i += 2; break;
        case 'n': out += '\n'; i += 2; break;
        case 'r': out += '\r'; i += 2; break;
        case 't': out += '\t'; i += 2; break;
        case 'v': out += '\v'; i += 2; break;
        case '\\': out += '\\'; i += 2; break;
        case '"': out += '"'; i += 2; break;
        case '\'': out += '\''; i += 2; break;
        case 'x': {
            auto v = hex(s.substr(i + 2, 2));
            if (!v || i + 4 > s.size()) return std::nullopt;
            out += char(*v);
            i += 4;
            break;
        }
        case 'u':
        case 'U': {
            std::size_t n = e == 'u' ? 4 : 8;
            auto v = hex(s.substr(i + 2, n));
            if (!v || i + 2 + n > s.size()) return std::nullopt;
            putRune(rune(*v));
            i += 2 + n;
            break;
        }
        default:
            if ('0' <= e && e <= '7') {
                if (i + 4 > s.size()) return std::nullopt;
                out += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
                i += 4;
                break;
            }
            return std::nullopt;
        }
    }
    return out;
}

std::vector<std::string_view> lines(std::string_view data) {
    std::vector<std::string_view> out;
    while (!data.empty()) {
        auto i = data.find('\n');
        out.push_back(data.substr(0, i));
        if (i == std::string_view::npos) break;
        data = data.substr(i + 1);
    }
    return out;
}

std::vector<int> parseResult(std::string_view res, int lineno) {
    if (res == "-") return {};
    std::vector<int> out;
    for (auto pair : strings::Split(string(res), " ")) {
        if (pair == "-") {
            out.push_back(-1);
            out.push_back(-1);
            continue;
        }
        auto [loStr, hiStr, _] = strings::Cut(pair, "-");
        auto [lo, err1] = strconv::Atoi(loStr);
        auto [hi, err2] = strconv::Atoi(hiStr);
        if (err1 || err2 || lo > hi) {
            ADD_FAILURE() << lineno << ": invalid pair " << pair;
            return {};
        }
        out.push_back(int(lo));
        out.push_back(int(hi));
    }
    return out;
}

bool isSingleBytes(std::string_view s) {
    for (char c : s) {
        if (byte(c) >= utf8::RuneSelf) return false;
    }
    return true;
}

TEST(GoRegexpExec, RE2Search) {
    std::vector<std::string> str, input;
    bool inStrings = false;
    // The expression and its \A(?:...)\z form, first-match and longest.
    ptr<regexp::Regexp> re, reLongest, refull, refullLongest;
    int nfail = 0, ncase = 0, lineno = 0;
    for (auto line : lines(re2Search)) {
        lineno++;
        if (line.empty()) FAIL() << lineno << ": unexpected blank line";
        if (line[0] == '#') continue;
        if ('A' <= line[0] && line[0] <= 'Z') continue;  // the test names
        if (line == "strings") {
            str.clear();
            inStrings = true;
        } else if (line == "regexps") {
            inStrings = false;
        } else if (line[0] == '"') {
            auto q = unquote(line);
            ASSERT_TRUE(q) << lineno << ": unquote " << line;
            if (inStrings) {
                str.push_back(*q);
                continue;
            }
            // Is this an expected string?
            ASSERT_TRUE(input.empty()) << lineno << ": out of sync: have " << input.size() << " strings left";
            auto [r, err] = regexp::Compile(*q);
            re = r;
            if (err) {
                if (err.Error() == "error parsing regexp: invalid escape sequence: `\\C`") {
                    // We don't and likely never will support \C; keep
                    // going.
                    continue;
                }
                ADD_FAILURE() << lineno << ": compile " << *q << ": " << err;
                if (++nfail >= 100) FAIL() << "stopping after " << nfail << " errors";
                continue;
            }
            reLongest = regexp::MustCompile(*q);
            reLongest->Longest();
            std::string full = R"(\A(?:)" + *q + R"()\z)";
            auto [rf, ferr] = regexp::Compile(full);
            ASSERT_FALSE(ferr) << lineno << ": compile full " << full << ": " << ferr;
            refull = rf;
            refullLongest = regexp::MustCompile(full);
            refullLongest->Longest();
            input = str;
        } else if (line[0] == '-' || ('0' <= line[0] && line[0] <= '9')) {
            // A sequence of match results.
            ncase++;
            if (!re) {
                // Failed to compile: skip results.
                continue;
            }
            ASSERT_FALSE(input.empty()) << lineno << ": out of sync: no input remaining";
            std::string text = input.front();
            input.erase(input.begin());
            if (!isSingleBytes(text) && re->String().view().find(R"(\B)") != std::string_view::npos) {
                // RE2's \B considers every byte position, so it sees
                // 'not word boundary' in the middle of UTF-8 sequences.
                // This package only considers the positions between
                // runes, so it disagrees. Skip those cases.
                continue;
            }
            auto res = strings::Split(string(line), ";");
            ASSERT_EQ(res.size(), 4u) << lineno << ": have " << res.size() << " test results, want 4";
            const regexp::Regexp* runs[4] = {refull.get(), re.get(), refullLongest.get(), reLongest.get()};
            const char* suffixes[4] = {"[full]", "", "[full,longest]", "[longest]"};
            for (std::size_t i = 0; i < 4; i++) {
                auto have = runs[i]->FindStringSubmatchIndex(string(text));
                auto want = parseResult(res[i], lineno);
                if (have != want) {
                    ADD_FAILURE() << lineno << ": " << re->String() << suffixes[i] << ".FindSubmatchIndex("
                                  << strconv::Quote(text) << ")";
                    if (++nfail >= 100) FAIL() << "stopping after " << nfail << " errors";
                    continue;
                }
                bool b = runs[i]->MatchString(string(text));
                if (b != !want.empty()) {
                    ADD_FAILURE() << lineno << ": " << re->String() << suffixes[i] << ".MatchString("
                                  << strconv::Quote(text) << ") = " << b;
                    if (++nfail >= 100) FAIL() << "stopping after " << nfail << " errors";
                }
            }
        } else {
            FAIL() << lineno << ": out of sync: " << line;
        }
    }
    ASSERT_TRUE(input.empty()) << "out of sync: have " << input.size() << " strings left at EOF";
    EXPECT_GT(ncase, 1000) << "cases tested";
}

struct fowlerResult {
    bool ok = false, compiled = false, matched = false;
    std::vector<int> pos;
};

fowlerResult parseFowlerResult(std::string_view s) {
    fowlerResult r;
    if (s.empty()) {
        // Match with no position information.
        return {true, true, true, {}};
    }
    if (s == "NOMATCH") return {true, true, false, {}};
    if ('A' <= s[0] && s[0] <= 'Z') {
        // All the other error codes are compile errors.
        return {true, false, false, {}};
    }
    r.compiled = true;
    std::vector<int> x;
    while (!s.empty()) {
        char end = ')';
        if (x.size() % 2 == 0) {
            if (s[0] != '(') return r;
            s.remove_prefix(1);
            end = ',';
        }
        std::size_t i = 0;
        while (i < s.size() && s[i] != end) i++;
        if (i == 0 || i == s.size()) return r;
        int v = -1;
        if (s.substr(0, i) != "?") {
            auto [n, err] = strconv::Atoi(s.substr(0, i));
            if (err) return r;
            v = int(n);
        }
        x.push_back(v);
        s.remove_prefix(i + 1);
    }
    if (x.size() % 2 != 0) return r;
    r.ok = true;
    r.matched = true;
    r.pos = x;
    return r;
}

void testFowler(std::string_view file, std::string_view data) {
    static auto notab = regexp::MustCompilePOSIX(R"([^\t]+)");
    int lineno = 0;
    std::string lastRegexp;
    for (auto line : lines(data)) {
        lineno++;
        if (line.empty() || line[0] == '#') continue;

        // Field 1: the regex(3) flags to apply, one character per
        // REG_feature flag. The test is skipped if REG_feature is not
        // supported by the implementation. If the first character is not
        // [BEASKLP] then the specification is a global control line. One
        // or more of [BEASKLP] may be specified; the test will be
        // repeated for each mode.
        auto parts = notab->FindAllString(string(line), -1);
        std::vector<std::string> field;
        bool skip = false;
        for (const auto& f : parts) {
            if (f == "NULL") {
                field.push_back("");
            } else if (f == "NIL") {
                skip = true;
                break;
            } else {
                field.push_back(f.str());
            }
        }
        if (skip || field.empty()) continue;

        std::string flag = field[0];
        switch (flag[0]) {
        case '?': case '&': case '|': case ';': case '{': case '}':
            // Ignore all the control operators. Just run everything.
            flag = flag.substr(1);
            if (flag.empty()) continue;
            break;
        case ':': {
            auto i = flag.find(':', 1);
            if (i == std::string::npos) continue;
            flag = flag.substr(i + 1);
            break;
        }
        case 'C': case 'N': case 'T': case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        case '8': case '9':
            continue;
        default:
            break;
        }

        // Can check field count now that we've handled the myriad comment
        // formats.
        if (field.size() < 4) {
            ADD_FAILURE() << file << ":" << lineno << ": too few fields: " << line;
            continue;
        }

        // Expand C escapes (a.k.a. Go escapes).
        if (flag.find('$') != std::string::npos) {
            if (auto u = unquote("\"" + field[1] + "\"")) {
                field[1] = *u;
            } else {
                ADD_FAILURE() << file << ":" << lineno << ": cannot unquote " << field[1];
            }
            if (auto u = unquote("\"" + field[2] + "\"")) {
                field[2] = *u;
            } else {
                ADD_FAILURE() << file << ":" << lineno << ": cannot unquote " << field[2];
            }
        }

        // Field 2: the regular expression pattern; SAME uses the pattern
        // from the previous specification.
        if (field[1] == "SAME") field[1] = lastRegexp;
        lastRegexp = field[1];

        // Field 3: the string to match.
        std::string text = field[2];

        // Field 4: the test outcome...
        auto r = parseFowlerResult(field[3]);
        if (!r.ok) {
            ADD_FAILURE() << file << ":" << lineno << ": cannot parse result " << field[3];
            continue;
        }

        // Field 5: optional comment appended to the report.
        for (char c : flag) {
            std::string pattern = field[1];
            syntax::Flags syn = syntax::POSIX | syntax::ClassNL;
            switch (c) {
            default:
                continue;
            case 'E':
                // extended regexp (what we support)
                break;
            case 'L':
                // literal
                pattern = regexp::QuoteMeta(string(pattern)).str();
                break;
            }
            if (flag.find('i') != std::string::npos) syn |= syntax::FoldCase;

            auto [re, err] = regexp::compile(pattern, syn, true);
            if (err) {
                EXPECT_FALSE(r.compiled) << file << ":" << lineno << ": " << pattern << " did not compile";
                continue;
            }
            if (!r.compiled) {
                ADD_FAILURE() << file << ":" << lineno << ": " << pattern << " should not compile";
                continue;
            }
            bool match = re->MatchString(string(text));
            if (match != r.matched) {
                ADD_FAILURE() << file << ":" << lineno << ": " << pattern << ".Match(" << strconv::Quote(text) << ") = " << match;
                continue;
            }
            auto have = re->FindStringSubmatchIndex(string(text));
            if (!have.empty() != match) {
                ADD_FAILURE() << file << ":" << lineno << ": " << pattern << ".Match = " << match << ", but FindSubmatchIndex disagrees";
                continue;
            }
            if (have.size() > r.pos.size()) have.resize(r.pos.size());
            EXPECT_EQ(have, r.pos) << file << ":" << lineno << ": " << pattern << ".FindSubmatchIndex(" << strconv::Quote(text) << ")";
        }
    }
}

TEST(GoRegexpExec, Fowler) {
    testFowler("basic.dat", basicDat);
    testFowler("nullsubexpr.dat", nullsubexprDat);
    testFowler("repetition.dat", repetitionDat);
}

TEST(GoRegexpExec, Longest) {
    auto [re, err] = regexp::Compile(R"(a(|b))");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(re->FindString("ab"), "a") << "first match";
    re->Longest();
    EXPECT_EQ(re->FindString("ab"), "ab") << "longest match";
}

TEST(GoRegexpExec, ProgramTooLongForBacktrack) {
    auto longRegex = regexp::MustCompile(R"((one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|fifteen|sixteen|seventeen|eighteen|nineteen|twenty|twentyone|twentytwo|twentythree|twentyfour|twentyfive|twentysix|twentyseven|twentyeight|twentynine|thirty|thirtyone|thirtytwo|thirtythree|thirtyfour|thirtyfive|thirtysix|thirtyseven|thirtyeight|thirtynine|forty|fortyone|fortytwo|fortythree|fortyfour|fortyfive|fortysix|fortyseven|fortyeight|fortynine|fifty|fiftyone|fiftytwo|fiftythree|fiftyfour|fiftyfive|fiftysix|fiftyseven|fiftyeight|fiftynine|sixty|sixtyone|sixtytwo|sixtythree|sixtyfour|sixtyfive|sixtysix|sixtyseven|sixtyeight|sixtynine|seventy|seventyone|seventytwo|seventythree|seventyfour|seventyfive|seventysix|seventyseven|seventyeight|seventynine|eighty|eightyone|eightytwo|eightythree|eightyfour|eightyfive|eightysix|eightyseven|eightyeight|eightynine|ninety|ninetyone|ninetytwo|ninetythree|ninetyfour|ninetyfive|ninetysix|ninetyseven|ninetyeight|ninetynine|onehundred))");
    EXPECT_TRUE(longRegex->MatchString("two"));
    EXPECT_FALSE(longRegex->MatchString("xxx"));
}

}  // namespace
