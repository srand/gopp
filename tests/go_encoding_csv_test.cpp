// Ported from Go 1.27.3's encoding/csv/reader_test.go and writer_test.go,
// with Go's test names. In readTests' inputs, § marks the start of a
// field, ¶ a record boundary and ∑ the position of an error, as in Go.
// errInvalidDelim is private; it's compared by its message.
#include <gopp/bytes.hpp>
#include <gopp/encoding/csv.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <string>
#include <vector>

using namespace gopp;

namespace {

const error errInvalidDelim = errors::New("csv: invalid field or comment delimiter");

error parseErr(const error& err) { return make_ptr<csv::ParseError>(0, 0, 0, err); }

std::string rep(std::string_view s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

struct readTest {
    std::string Name;
    std::string Input;
    std::vector<std::vector<std::string>> Output;
    std::vector<error> Errors;

    // These fields are copied into the Reader.
    rune Comma = 0;
    rune Comment = 0;
    bool UseFieldsPerRecord = false;  // false (default) means FieldsPerRecord is -1
    int FieldsPerRecord = 0;
    bool LazyQuotes = false;
    bool TrimLeadingSpace = false;
    bool ReuseRecord = false;
};

// Built on first use: csv's errors are initialized by then.
const std::vector<readTest>& readTests() {
    // clang-format off
    static const std::vector<readTest> tests = {
        [] {
            readTest t;
            t.Name = "Simple";
            t.Input = "§a,§b,§c\n";
            t.Output = {{"a", "b", "c"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "CRLF";
            t.Input = "§a,§b\r\n¶§c,§d\r\n";
            t.Output = {{"a", "b"}, {"c", "d"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BareCR";
            t.Input = "§a,§b\rc,§d\r\n";
            t.Output = {{"a", "b\rc", "d"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "RFC4180test";
            t.Input = "§#field1,§field2,§field3\n¶§\"aaa\",§\"bb\nb\",§\"ccc\"\n¶§\"a,a\",§\"b\"\"bb\",§\"ccc\"\n¶§zzz,§yyy,§xxx\n";
            t.Output = {
                {"#field1", "field2", "field3"},
                {"aaa", "bb\nb", "ccc"},
                {"a,a", "b\"bb", "ccc"},
                {"zzz", "yyy", "xxx"},
            };
            t.UseFieldsPerRecord = true;
            t.FieldsPerRecord = 0;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NoEOLTest";
            t.Input = "§a,§b,§c";
            t.Output = {{"a", "b", "c"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "Semicolon";
            t.Input = "§a;§b;§c\n";
            t.Output = {{"a", "b", "c"}};
            t.Comma = U';';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "MultiLine";
            t.Input = "§\"two\nline\",§\"one line\",§\"three\nline\nfield\"";
            t.Output = {{"two\nline", "one line", "three\nline\nfield"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BlankLine";
            t.Input = "§a,§b,§c\n\n¶§d,§e,§f\n\n";
            t.Output = {
                {"a", "b", "c"},
                {"d", "e", "f"},
            };
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BlankLineFieldCount";
            t.Input = "§a,§b,§c\n\n¶§d,§e,§f\n\n";
            t.Output = {
                {"a", "b", "c"},
                {"d", "e", "f"},
            };
            t.UseFieldsPerRecord = true;
            t.FieldsPerRecord = 0;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrimSpace";
            t.Input = " §a,  §b,   §c\n";
            t.Output = {{"a", "b", "c"}};
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "LeadingSpace";
            t.Input = "§ a,§  b,§   c\n";
            t.Output = {{" a", "  b", "   c"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "Comment";
            t.Input = "#1,2,3\n§a,§b,§c\n#comment";
            t.Output = {{"a", "b", "c"}};
            t.Comment = U'#';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NoComment";
            t.Input = "§#1,§2,§3\n¶§a,§b,§c";
            t.Output = {{"#1", "2", "3"}, {"a", "b", "c"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "LazyQuotes";
            t.Input = "§a \"word\",§\"1\"2\",§a\",§\"b";
            t.Output = {{"a \"word\"", "1\"2", "a\"", "b"}};
            t.LazyQuotes = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BareQuotes";
            t.Input = "§a \"word\",§\"1\"2\",§a\"";
            t.Output = {{"a \"word\"", "1\"2", "a\""}};
            t.LazyQuotes = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BareDoubleQuotes";
            t.Input = "§a\"\"b,§c";
            t.Output = {{"a\"\"b", "c"}};
            t.LazyQuotes = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadDoubleQuotes";
            t.Input = "§a∑\"\"b,c";
            t.Errors = {parseErr(csv::ErrBareQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrimQuote";
            t.Input = " §\"a\",§\" b\",§c";
            t.Output = {{"a", " b", "c"}};
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadBareQuote";
            t.Input = "§a ∑\"word\",\"b\"";
            t.Errors = {parseErr(csv::ErrBareQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadTrailingQuote";
            t.Input = "§\"a word\",b∑\"";
            t.Errors = {parseErr(csv::ErrBareQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "ExtraneousQuote";
            t.Input = "§\"a ∑\"word\",\"b\"";
            t.Errors = {parseErr(csv::ErrQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadFieldCount";
            t.Input = "§a,§b,§c\n¶∑§d,§e";
            t.Errors = {nullptr, parseErr(csv::ErrFieldCount)};
            t.Output = {{"a", "b", "c"}, {"d", "e"}};
            t.UseFieldsPerRecord = true;
            t.FieldsPerRecord = 0;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadFieldCountMultiple";
            t.Input = "§a,§b,§c\n¶∑§d,§e\n¶∑§f";
            t.Errors = {nullptr, parseErr(csv::ErrFieldCount), parseErr(csv::ErrFieldCount)};
            t.Output = {{"a", "b", "c"}, {"d", "e"}, {"f"}};
            t.UseFieldsPerRecord = true;
            t.FieldsPerRecord = 0;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadFieldCount1";
            t.Input = "§∑a,§b,§c";
            t.Errors = {parseErr(csv::ErrFieldCount)};
            t.Output = {{"a", "b", "c"}};
            t.UseFieldsPerRecord = true;
            t.FieldsPerRecord = 2;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCount";
            t.Input = "§a,§b,§c\n¶§d,§e";
            t.Output = {{"a", "b", "c"}, {"d", "e"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaEOF";
            t.Input = "§a,§b,§c,§";
            t.Output = {{"a", "b", "c", ""}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaEOL";
            t.Input = "§a,§b,§c,§\n";
            t.Output = {{"a", "b", "c", ""}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaSpaceEOF";
            t.Input = "§a,§b,§c, §";
            t.Output = {{"a", "b", "c", ""}};
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaSpaceEOL";
            t.Input = "§a,§b,§c, §\n";
            t.Output = {{"a", "b", "c", ""}};
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaLine3";
            t.Input = "§a,§b,§c\n¶§d,§e,§f\n¶§g,§hi,§";
            t.Output = {{"a", "b", "c"}, {"d", "e", "f"}, {"g", "hi", ""}};
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NotTrailingComma3";
            t.Input = "§a,§b,§c,§ \n";
            t.Output = {{"a", "b", "c", " "}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "CommaFieldTest";
            t.Input = "§x,§y,§z,§w\n¶§x,§y,§z,§\n¶§x,§y,§,§\n¶§x,§,§,§\n¶§,§,§,§\n¶§\"x\",§\"y\",§\"z\",§\"w\"\n¶§\"x\",§\"y\",§\"z\",§\"\"\n¶§\"x\",§\"y\",§\"\",§\"\"\n¶§\"x\",§\"\",§\"\",§\"\"\n¶§\"\",§\"\",§\"\",§\"\"\n";
            t.Output = {
                {"x", "y", "z", "w"},
                {"x", "y", "z", ""},
                {"x", "y", "", ""},
                {"x", "", "", ""},
                {"", "", "", ""},
                {"x", "y", "z", "w"},
                {"x", "y", "z", ""},
                {"x", "y", "", ""},
                {"x", "", "", ""},
                {"", "", "", ""},
            };
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCommaIneffective1";
            t.Input = "§a,§b,§\n¶§c,§d,§e";
            t.Output = {
                {"a", "b", ""},
                {"c", "d", "e"},
            };
            t.TrimLeadingSpace = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "ReadAllReuseRecord";
            t.Input = "§a,§b\n¶§c,§d";
            t.Output = {
                {"a", "b"},
                {"c", "d"},
            };
            t.ReuseRecord = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "StartLine1"; // Issue 19019
            t.Input = "§a,\"b\nc∑\"d,e";
            t.Errors = {parseErr(csv::ErrQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "StartLine2";
            t.Input = "§a,§b\n¶§\"d\n\n,e∑";
            t.Errors = {nullptr, parseErr(csv::ErrQuote)};
            t.Output = {{"a", "b"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "CRLFInQuotedField"; // Issue 21201
            t.Input = "§A,§\"Hello\r\nHi\",§B\r\n";
            t.Output = {
                {"A", "Hello\nHi", "B"},
            };
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BinaryBlobField"; // Issue 19410
            t.Input = "§x09A\264\034,§aktau";
            t.Output = {{"x09A\264\034", "aktau"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "TrailingCR";
            t.Input = "§field1,§field2\r";
            t.Output = {{"field1", "field2"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "QuotedTrailingCR";
            t.Input = "§\"field\"\r";
            t.Output = {{"field"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "QuotedTrailingCRCR";
            t.Input = "§\"field∑\"\r\r";
            t.Errors = {parseErr(csv::ErrQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCR";
            t.Input = "§field\rfield\r";
            t.Output = {{"field\rfield"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCRCR";
            t.Input = "§field\r\rfield\r\r";
            t.Output = {{"field\r\rfield\r"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCRCRLF";
            t.Input = "§field\r\r\n¶§field\r\r\n";
            t.Output = {{"field\r"}, {"field\r"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCRCRLFCR";
            t.Input = "§field\r\r\n¶§\rfield\r\r\n\r";
            t.Output = {{"field\r"}, {"\rfield\r"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "FieldCRCRLFCRCR";
            t.Input = "§field\r\r\n¶§\r\rfield\r\r\n¶§\r\r";
            t.Output = {{"field\r"}, {"\r\rfield\r"}, {"\r"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "MultiFieldCRCRLFCRCR";
            t.Input = "§field1,§field2\r\r\n¶§\r\rfield1,§field2\r\r\n¶§\r\r,§";
            t.Output = {
                {"field1", "field2\r"},
                {"\r\rfield1", "field2\r"},
                {"\r\r", ""},
            };
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NonASCIICommaAndComment";
            t.Input = "§a£§b,c£ \t§d,e\n€ comment\n";
            t.Output = {{"a", "b,c", "d,e"}};
            t.TrimLeadingSpace = true;
            t.Comma = U'£';
            t.Comment = U'€';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NonASCIICommaAndCommentWithQuotes";
            t.Input = "§a€§\"  b,\"€§ c\nλ comment\n";
            t.Output = {{"a", "  b,", " c"}};
            t.Comma = U'€';
            t.Comment = U'λ';
            return t;
        }(),
        [] {
            readTest t;
            // λ and θ start with the same byte.
            // This tests that the parser doesn't confuse such characters.
            t.Name = "NonASCIICommaConfusion";
            t.Input = "§\"abθcd\"λ§efθgh";
            t.Output = {{"abθcd", "efθgh"}};
            t.Comma = U'λ';
            t.Comment = U'€';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "NonASCIICommentConfusion";
            t.Input = "§λ\n¶§λ\nθ\n¶§λ\n";
            t.Output = {{"λ"}, {"λ"}, {"λ"}};
            t.Comment = U'θ';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "QuotedFieldMultipleLF";
            t.Input = "§\"\n\n\n\n\"";
            t.Output = {{"\n\n\n\n"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "MultipleCRLF";
            t.Input = "\r\n\r\n\r\n\r\n";
            return t;
        }(),
        [] {
            readTest t;
            // The implementation may read each line in several chunks if it doesn't fit entirely
            // in the read buffer, so we should test the code to handle that condition.
            t.Name = "HugeLines";
            t.Input = rep("#ignore\n", 10000) + "§" + rep("@", 5000) + ",§" + rep("*", 5000);
            t.Output = {{rep("@", 5000), rep("*", 5000)}};
            t.Comment = U'#';
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "QuoteWithTrailingCRLF";
            t.Input = "§\"foo∑\"bar\"\r\n";
            t.Errors = {parseErr(csv::ErrQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "LazyQuoteWithTrailingCRLF";
            t.Input = "§\"foo\"bar\"\r\n";
            t.Output = {{"foo\"bar"}};
            t.LazyQuotes = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "DoubleQuoteWithTrailingCRLF";
            t.Input = "§\"foo\"\"bar\"\r\n";
            t.Output = {{"foo\"bar"}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "EvenQuotes";
            t.Input = "§\"\"\"\"\"\"\"\"";
            t.Output = {{"\"\"\""}};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "OddQuotes";
            t.Input = "§\"\"\"\"\"\"\"∑";
            t.Errors = {parseErr(csv::ErrQuote)};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "LazyOddQuotes";
            t.Input = "§\"\"\"\"\"\"\"";
            t.Output = {{"\"\"\""}};
            t.LazyQuotes = true;
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComma1";
            t.Comma = U'\n';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComma2";
            t.Comma = U'\r';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComma3";
            t.Comma = U'"';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComma4";
            t.Comma = utf8::RuneError;
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComment1";
            t.Comment = U'\n';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComment2";
            t.Comment = U'\r';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadComment3";
            t.Comment = utf8::RuneError;
            t.Errors = {errInvalidDelim};
            return t;
        }(),
        [] {
            readTest t;
            t.Name = "BadCommaComment";
            t.Comma = U'X';
            t.Comment = U'X';
            t.Errors = {errInvalidDelim};
            return t;
        }(),
    };
    // clang-format on
    return tests;
}

using positionsT = std::vector<std::vector<std::array<int, 2>>>;

// The expected field positions of all the fields in text, the positions of
// any errors, and the text with the position markers removed.
std::tuple<positionsT, std::map<int, std::array<int, 2>>, std::string> makePositions(std::string_view text) {
    std::string buf;
    positionsT positions;
    std::map<int, std::array<int, 2>> errPositions;
    int line = 1, col = 1;
    int recNum = 0;

    while (!text.empty()) {
        auto [r, size] = utf8::DecodeRuneInString(text);
        switch (r) {
            case '\n':
                line++;
                col = 1;
                buf += '\n';
                break;
            case U'§':
                if (positions.empty()) positions.emplace_back();
                positions.back().push_back({line, col});
                break;
            case U'¶':
                positions.emplace_back();
                recNum++;
                break;
            case U'∑':
                errPositions[recNum] = {line, col};
                break;
            default:
                buf += text.substr(0, std::size_t(size));
                col += size;
        }
        text = text.substr(std::size_t(size));
    }
    return {positions, errPositions, buf};
}

// err with the position adjusted according to its record's index in
// positions.
error errorWithPosition(const error& err, int recNum, const positionsT& positions,
                        const std::map<int, std::array<int, 2>>& errPositions) {
    auto pe = errors::As<csv::ParseError>(err);
    if (!pe) return err;
    if (std::size_t(recNum) >= positions.size()) panic("no positions found for error");
    auto it = errPositions.find(recNum);
    if (it == errPositions.end()) panic("no error position found for error");
    return make_ptr<csv::ParseError>(positions[std::size_t(recNum)][0][0], it->second[0], it->second[1], pe->Err);
}

// The first non-nil error in errs, with its position adjusted.
error firstError(const std::vector<error>& errs, const positionsT& positions,
                 const std::map<int, std::array<int, 2>>& errPositions) {
    for (std::size_t i = 0; i < errs.size(); i++) {
        if (errs[i]) return errorWithPosition(errs[i], int(i), positions, errPositions);
    }
    return nullptr;
}

// Go's reflect.DeepEqual of the errors.
bool sameError(const error& got, const error& want) {
    if (!got || !want) return !got && !want;
    auto g = errors::As<csv::ParseError>(got);
    auto w = errors::As<csv::ParseError>(want);
    if (g || w) {
        return g && w && g->StartLine == w->StartLine && g->Line == w->Line && g->Column == w->Column &&
               g->Err == w->Err;
    }
    return got == want || got->Error() == want->Error();
}

std::string str(const error& err) { return err ? err->Error().str() : "<nil>"; }

std::vector<std::string> strs(const std::vector<string>& v) {
    std::vector<std::string> r;
    for (const auto& s : v) r.push_back(s.str());
    return r;
}

}  // namespace

TEST(GoEncodingCSV, Read) {
    auto newReader = [](const readTest& tt) {
        auto [positions, errPositions, input] = makePositions(tt.Input);
        auto r = csv::NewReader(strings::NewReader(input));
        if (tt.Comma != 0) r->Comma = tt.Comma;
        r->Comment = tt.Comment;
        r->FieldsPerRecord = tt.UseFieldsPerRecord ? tt.FieldsPerRecord : -1;
        r->LazyQuotes = tt.LazyQuotes;
        r->TrimLeadingSpace = tt.TrimLeadingSpace;
        r->ReuseRecord = tt.ReuseRecord;
        return std::tuple{r, positions, errPositions, input};
    };

    for (const auto& tt : readTests()) {
        SCOPED_TRACE(tt.Name);
        auto [r, positions, errPositions, input] = newReader(tt);
        auto [out, err] = r->ReadAll();
        if (auto wantErr = firstError(tt.Errors, positions, errPositions)) {
            ASSERT_TRUE(sameError(err, wantErr)) << "ReadAll() error mismatch:\ngot  " << str(err) << "\nwant "
                                                 << str(wantErr);
            ASSERT_TRUE(out.empty());
        } else {
            ASSERT_EQ(err, nullptr) << "unexpected ReadAll() error: " << str(err);
            std::vector<std::vector<std::string>> got;
            for (const auto& rec : out) got.push_back(strs(rec));
            ASSERT_EQ(got, tt.Output);
        }

        // Check the input offset after ReadAll.
        if (!err) {
            EXPECT_EQ(int64(input.size()), r->InputOffset()) << "wrong input offset after ReadAll()";
        }

        // Check field and error positions.
        r = std::get<0>(newReader(tt));
        for (int recNum = 0;; recNum++) {
            auto [rec, err] = r->Read();
            error wantErr;
            if (std::size_t(recNum) < tt.Errors.size() && tt.Errors[std::size_t(recNum)]) {
                wantErr = errorWithPosition(tt.Errors[std::size_t(recNum)], recNum, positions, errPositions);
            } else if (std::size_t(recNum) >= tt.Output.size()) {
                wantErr = io::EOF_;
            }
            ASSERT_TRUE(sameError(err, wantErr))
                << "Read() error at record " << recNum << ":\ngot " << str(err) << "\nwant " << str(wantErr);
            // ErrFieldCount is explicitly non-fatal.
            if (err && !errors::Is(err, csv::ErrFieldCount)) {
                ASSERT_GE(std::size_t(recNum), tt.Output.size()) << "need more records";
                break;
            }
            EXPECT_EQ(strs(rec), tt.Output[std::size_t(recNum)]) << "Read vs ReadAll mismatch";
            const auto& pos = positions[std::size_t(recNum)];
            ASSERT_EQ(pos.size(), rec.size()) << "mismatched position length at record " << recNum;
            for (std::size_t i = 0; i < rec.size(); i++) {
                auto [line, col] = r->FieldPos(int(i));
                EXPECT_EQ((std::array<int, 2>{line, col}), pos[i])
                    << "position mismatch at record " << recNum << ", field " << i;
            }
        }
    }
}

TEST(EncodingCSV, ParseErrorString) {
    EXPECT_EQ(csv::ParseError(2, 2, 5, csv::ErrQuote).Error(),
              "parse error on line 2, column 5: extraneous or missing \" in quoted-field");
    EXPECT_EQ(csv::ParseError(1, 3, 2, csv::ErrBareQuote).Error(),
              "record on line 1; parse error on line 3, column 2: bare \" in non-quoted-field");
    EXPECT_EQ(csv::ParseError(4, 4, 1, csv::ErrFieldCount).Error(), "record on line 4: wrong number of fields");
}

TEST(EncodingCSV, ReuseRecord) {
    auto r = csv::NewReader(strings::NewReader("a,b\nc,d\n"));
    r->ReuseRecord = true;
    auto [rec1, err1] = r->Read();
    ASSERT_EQ(err1, nullptr);
    auto [rec2, err2] = r->Read();
    ASSERT_EQ(err2, nullptr);
    EXPECT_EQ(strs(rec1), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(strs(rec2), (std::vector<std::string>{"c", "d"}));
    EXPECT_EQ(r->Read().second, io::EOF_);
}

// ---------------------------------------------------------------------------
// writer_test.go

namespace {

struct writeTest {
    std::vector<std::vector<string>> Input{};
    std::string Output{};
    error Error = nullptr;
    bool UseCRLF = false;
    rune Comma = 0;
};

const std::vector<writeTest> writeTests = {
    {.Input = {{"abc"}}, .Output = "abc\n"},
    {.Input = {{"abc"}}, .Output = "abc\r\n", .UseCRLF = true},
    {.Input = {{R"("abc")"}}, .Output = R"("""abc""")" "\n"},
    {.Input = {{R"(a"b)"}}, .Output = R"("a""b")" "\n"},
    {.Input = {{R"("a"b")"}}, .Output = R"("""a""b""")" "\n"},
    {.Input = {{" abc"}}, .Output = R"(" abc")" "\n"},
    {.Input = {{"abc,def"}}, .Output = R"("abc,def")" "\n"},
    {.Input = {{"abc", "def"}}, .Output = "abc,def\n"},
    {.Input = {{"abc"}, {"def"}}, .Output = "abc\ndef\n"},
    {.Input = {{"abc\ndef"}}, .Output = "\"abc\ndef\"\n"},
    {.Input = {{"abc\ndef"}}, .Output = "\"abc\r\ndef\"\r\n", .UseCRLF = true},
    {.Input = {{"abc\rdef"}}, .Output = "\"abcdef\"\r\n", .UseCRLF = true},
    {.Input = {{"abc\rdef"}}, .Output = "\"abc\rdef\"\n", .UseCRLF = false},
    {.Input = {{""}}, .Output = "\n"},
    {.Input = {{"", ""}}, .Output = ",\n"},
    {.Input = {{"", "", ""}}, .Output = ",,\n"},
    {.Input = {{"", "", "a"}}, .Output = ",,a\n"},
    {.Input = {{"", "a", ""}}, .Output = ",a,\n"},
    {.Input = {{"", "a", "a"}}, .Output = ",a,a\n"},
    {.Input = {{"a", "", ""}}, .Output = "a,,\n"},
    {.Input = {{"a", "", "a"}}, .Output = "a,,a\n"},
    {.Input = {{"a", "a", ""}}, .Output = "a,a,\n"},
    {.Input = {{"a", "a", "a"}}, .Output = "a,a,a\n"},
    {.Input = {{R"(\.)"}}, .Output = "\"\\.\"\n"},
    {.Input = {{"x09\x41\264\034", "aktau"}}, .Output = "x09\x41\264\034,aktau\n"},
    {.Input = {{",x09\x41\264\034", "aktau"}}, .Output = "\",x09\x41\264\034\",aktau\n"},
    {.Input = {{"a", "a", ""}}, .Output = "a|a|\n", .Comma = '|'},
    {.Input = {{",", ",", ""}}, .Output = ",|,|\n", .Comma = '|'},
    {.Input = {{"foo"}}, .Error = errInvalidDelim, .Comma = '"'},
};

struct errorWriter final : io::Writer {
    std::pair<isize, error> Write(std::span<const byte>) override { return {0, errors::New("Test")}; }
};

}  // namespace

TEST(GoEncodingCSV, Write) {
    for (std::size_t n = 0; n < writeTests.size(); n++) {
        const auto& tt = writeTests[n];
        auto b = make_ptr<bytes::Buffer>();
        auto f = csv::NewWriter(b);
        f->UseCRLF = tt.UseCRLF;
        if (tt.Comma != 0) f->Comma = tt.Comma;
        auto err = f->WriteAll(tt.Input);
        EXPECT_TRUE(sameError(err, tt.Error)) << "#" << n << ": unexpected error: " << str(err);
        EXPECT_EQ(asStringView(b->Bytes()), tt.Output) << "#" << n;
    }
}

TEST(GoEncodingCSV, Error) {
    auto b = make_ptr<bytes::Buffer>();
    auto f = csv::NewWriter(b);
    (void)f->Write({"abc"});
    f->Flush();
    EXPECT_EQ(f->Error(), nullptr);

    f = csv::NewWriter(make_ptr<errorWriter>());
    (void)f->Write({"abc"});
    f->Flush();
    EXPECT_NE(f->Error(), nullptr);
}
