// The encoding/csv package: comma-separated values as RFC 4180 has them,
// after Go 1.27.3.
//
//     auto r = csv::NewReader(strings::NewReader("name,qty\n\"Gopher, G.\",3\n"));
//     auto [records, err] = r->ReadAll();   // {{"name", "qty"}, {"Gopher, G.", "3"}}
//
//     auto w = csv::NewWriter(file);
//     w->Write({"a", "b \"quoted\""});      // a,"b ""quoted"""
//     w->Flush();
//
// Records are std::vector<string>. The fields of a record share one buffer.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::csv {

// csv.ParseError: a parse error, and where it happened.
struct ParseError : errorInterface {
    ParseError() = default;
    ParseError(int startLine, int line, int column, error err)
        : StartLine(startLine), Line(line), Column(column), Err(std::move(err)) {}
    string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }

    int StartLine = 0;  // the line the record starts on
    int Line = 0;       // the line the error is on
    int Column = 0;     // the column (1-based byte index) of the error
    error Err;          // the error itself
};

// The errors of a ParseError.
extern const error ErrBareQuote;   // bare " in non-quoted-field
extern const error ErrQuote;       // extraneous or missing " in quoted-field
extern const error ErrFieldCount;  // wrong number of fields
// Deprecated in Go, and no longer returned.
extern const error ErrTrailingComma;

// csv.Reader: reads records from CSV text. A record is a line of fields
// separated by Comma; a field may be quoted ("..."), and then hold commas,
// newlines and doubled quotes (""). Carriage returns before newlines are
// dropped, outside quoted fields too. Empty lines are skipped.
class Reader : public object {
public:
    explicit Reader(ptr<io::Reader> r);

    // The field delimiter, ',' by default. Not \r, \n, " or an invalid rune.
    rune Comma = ',';
    // If not 0, lines starting with it are comments, and skipped.
    rune Comment = 0;
    // The number of fields a record must have: if positive, that number;
    // if 0, the number of the first record's; if negative, any.
    int FieldsPerRecord = 0;
    // A quote may appear in an unquoted field, and a non-doubled quote in
    // a quoted field.
    bool LazyQuotes = false;
    // Leading white space in a field is ignored, even with a non-space
    // Comma.
    bool TrimLeadingSpace = false;
    // Read reuses the vector it returned last; Go's backing array.
    bool ReuseRecord = false;
    // Deprecated in Go: ignored.
    bool TrailingComma = false;

    // The next record. With ReadAll, io::EOF_ at the end. A record with
    // the wrong number of fields comes with a ParseError wrapping
    // ErrFieldCount; other errors come with what was read.
    std::pair<std::vector<string>, error> Read();
    // The line and column of the start of field field of the last record
    // Read returned (1-based); panics if out of range.
    std::pair<int, int> FieldPos(int field) const;
    // The byte offset in the input of the end of the last record read.
    int64 InputOffset() const noexcept { return offset_; }
    // All the remaining records; no error at the end of the input.
    std::pair<std::vector<std::vector<string>>, error> ReadAll();

private:
    struct position {
        int line, col;
    };
    std::pair<std::string_view, error> readLine();
    std::pair<std::vector<string>, error> readRecord(std::vector<string> dst);

    ptr<bufio::Reader> r_;
    int numLine_ = 0;
    int64 offset_ = 0;
    std::string rawBuffer_;
    std::string recordBuffer_;
    std::vector<std::size_t> fieldIndexes_;
    std::vector<position> fieldPositions_;
    std::vector<string> lastRecord_;
};

// csv.NewReader: a Reader of r.
ptr<Reader> NewReader(ptr<io::Reader> r);

// csv.Writer: writes records as CSV, quoting fields as needed, through a
// bufio::Writer: call Flush at the end, and Error to see whether the
// writes failed.
class Writer : public object {
public:
    explicit Writer(ptr<io::Writer> w);

    // The field delimiter, ',' by default.
    rune Comma = ',';
    // Lines end in \r\n instead of \n.
    bool UseCRLF = false;

    // Writes a record, quoting fields that need it.
    error Write(const std::vector<string>& record);
    // Writes the buffered data to the underlying writer.
    void Flush();
    // The error of a previous Write or Flush.
    error Error();
    // Writes records, then flushes.
    error WriteAll(const std::vector<std::vector<string>>& records);

private:
    bool fieldNeedsQuotes(std::string_view field) const;

    ptr<bufio::Writer> w_;
};

// csv.NewWriter: a Writer to w.
ptr<Writer> NewWriter(ptr<io::Writer> w);

}  // namespace gopp::csv
