// encoding/csv, ported from Go 1.27.3's encoding/csv/reader.go and
// writer.go.
#include <gopp/encoding/csv.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <format>

namespace gopp::csv {

namespace {

const error errInvalidDelim = errors::New("csv: invalid field or comment delimiter");

string errText(const error& err) { return err ? err->Error() : string::fromStatic("<nil>"); }

bool validDelim(rune r) {
    return r != 0 && r != '"' && r != '\r' && r != '\n' && utf8::ValidRune(r) && r != utf8::RuneError;
}

std::size_t lengthNL(std::string_view b) { return !b.empty() && b.back() == '\n' ? 1 : 0; }

rune nextRune(std::string_view b) { return utf8::DecodeRuneInString(b).first; }

std::string encodeRune(rune r) {
    byte buf[utf8::UTFMax];
    int n = utf8::EncodeRune(buf, r);
    return std::string(reinterpret_cast<const char*>(buf), std::size_t(n));
}

}  // namespace

const error ErrBareQuote = errors::New("bare \" in non-quoted-field");
const error ErrQuote = errors::New("extraneous or missing \" in quoted-field");
const error ErrFieldCount = errors::New("wrong number of fields");
const error ErrTrailingComma = errors::New("extra delimiter at end of line");

string ParseError::Error() const {
    if (Err == ErrFieldCount) return std::format("record on line {}: {}", Line, errText(Err).view());
    if (StartLine != Line) {
        return std::format("record on line {}; parse error on line {}, column {}: {}", StartLine, Line, Column,
                           errText(Err).view());
    }
    return std::format("parse error on line {}, column {}: {}", Line, Column, errText(Err).view());
}

// ---------------------------------------------------------------------------
// Reader

Reader::Reader(ptr<io::Reader> r) : r_(make_ptr<bufio::Reader>(std::move(r))) {}

ptr<Reader> NewReader(ptr<io::Reader> r) { return make_ptr<Reader>(std::move(r)); }

std::pair<std::vector<string>, error> Reader::Read() {
    if (ReuseRecord) {
        auto [record, err] = readRecord(std::move(lastRecord_));
        lastRecord_ = record;
        return {std::move(record), err};
    }
    return readRecord({});
}

std::pair<int, int> Reader::FieldPos(int field) const {
    if (field < 0 || std::size_t(field) >= fieldPositions_.size()) panic("out of range index passed to FieldPos");
    const auto& p = fieldPositions_[std::size_t(field)];
    return {p.line, p.col};
}

std::pair<std::vector<std::vector<string>>, error> Reader::ReadAll() {
    std::vector<std::vector<string>> records;
    for (;;) {
        auto [record, err] = readRecord({});
        if (err == io::EOF_) return {std::move(records), nullptr};
        if (err) return {{}, err};
        records.push_back(std::move(record));
    }
}

// The next line, with its \n (unless at the end of the input), \r\n
// normalized to \n. The view is valid until the next call.
std::pair<std::string_view, error> Reader::readLine() {
    rawBuffer_.clear();
    auto [line, err] = r_->ReadSlice('\n');
    rawBuffer_.append(reinterpret_cast<const char*>(line.data()), line.size());
    while (err == bufio::ErrBufferFull) {
        std::tie(line, err) = r_->ReadSlice('\n');
        rawBuffer_.append(reinterpret_cast<const char*>(line.data()), line.size());
    }
    std::size_t readSize = rawBuffer_.size();
    if (readSize > 0 && err == io::EOF_) {
        err = nullptr;
        // For backwards compatibility, drop a trailing \r before EOF.
        if (rawBuffer_.back() == '\r') rawBuffer_.pop_back();
    }
    numLine_++;
    offset_ += int64(readSize);
    // Normalize \r\n to \n on all input lines.
    if (std::size_t n = rawBuffer_.size(); n >= 2 && rawBuffer_[n - 2] == '\r' && rawBuffer_[n - 1] == '\n') {
        rawBuffer_[n - 2] = '\n';
        rawBuffer_.pop_back();
    }
    return {rawBuffer_, err};
}

std::pair<std::vector<string>, error> Reader::readRecord(std::vector<string> dst) {
    if (Comma == Comment || !validDelim(Comma) || (Comment != 0 && !validDelim(Comment))) {
        return {{}, errInvalidDelim};
    }

    // Read the line, skipping comments and empty lines.
    std::string_view line;
    error errRead;
    while (!errRead) {
        std::tie(line, errRead) = readLine();
        if (Comment != 0 && nextRune(line) == Comment) {
            line = {};
            continue;  // skip comment lines
        }
        if (!errRead && line.size() == lengthNL(line)) {
            line = {};
            continue;  // skip empty lines
        }
        break;
    }
    if (errRead == io::EOF_) return {{}, errRead};

    // Parse each field in the record.
    error err;
    constexpr std::size_t quoteLen = 1;
    const std::string comma = encodeRune(Comma);
    const std::size_t commaLen = comma.size();
    int recLine = numLine_;  // the starting line of the record
    recordBuffer_.clear();
    fieldIndexes_.clear();
    fieldPositions_.clear();
    position pos{numLine_, 1};
    for (bool more = true; more;) {
        if (TrimLeadingSpace) {
            std::size_t i = 0;
            while (i < line.size()) {
                auto [r, size] = utf8::DecodeRuneInString(line.substr(i));
                if (!unicode::IsSpace(r)) break;
                i += std::size_t(size);
            }
            if (i == line.size()) pos.col -= int(lengthNL(line));
            line = line.substr(i);
            pos.col += int(i);
        }
        if (line.empty() || line[0] != '"') {
            // A non-quoted field.
            std::size_t i = line.find(comma);
            std::string_view field = line;
            if (i != line.npos) {
                field = field.substr(0, i);
            } else {
                field = field.substr(0, field.size() - lengthNL(field));
            }
            // Check to make sure a quote does not appear in the field.
            if (!LazyQuotes) {
                if (std::size_t j = field.find('"'); j != field.npos) {
                    err = make_ptr<ParseError>(recLine, numLine_, pos.col + int(j), ErrBareQuote);
                    break;
                }
            }
            recordBuffer_ += field;
            fieldIndexes_.push_back(recordBuffer_.size());
            fieldPositions_.push_back(pos);
            if (i != line.npos) {
                line = line.substr(i + commaLen);
                pos.col += int(i + commaLen);
                continue;
            }
            break;
        }
        // A quoted field.
        position fieldPos = pos;
        line = line.substr(quoteLen);
        pos.col += int(quoteLen);
        for (;;) {
            std::size_t i = line.find('"');
            if (i != line.npos) {
                // Hit the next quote.
                recordBuffer_ += line.substr(0, i);
                line = line.substr(i + quoteLen);
                pos.col += int(i + quoteLen);
                rune rn = nextRune(line);
                if (rn == '"') {
                    // `""` sequence (append quote).
                    recordBuffer_ += '"';
                    line = line.substr(quoteLen);
                    pos.col += int(quoteLen);
                } else if (rn == Comma) {
                    // `",` sequence (end of field).
                    line = line.substr(commaLen);
                    pos.col += int(commaLen);
                    fieldIndexes_.push_back(recordBuffer_.size());
                    fieldPositions_.push_back(fieldPos);
                    break;  // the next field
                } else if (lengthNL(line) == line.size()) {
                    // `"\n` sequence (end of line).
                    fieldIndexes_.push_back(recordBuffer_.size());
                    fieldPositions_.push_back(fieldPos);
                    more = false;
                    break;
                } else if (LazyQuotes) {
                    // `"` sequence (bare quote).
                    recordBuffer_ += '"';
                } else {
                    // `"*` sequence (invalid non-escaped quote).
                    err = make_ptr<ParseError>(recLine, numLine_, pos.col - int(quoteLen), ErrQuote);
                    more = false;
                    break;
                }
            } else if (!line.empty()) {
                // Hit the end of the line (copy all data so far).
                recordBuffer_ += line;
                if (errRead) {
                    more = false;
                    break;
                }
                pos.col += int(line.size());
                // Abrupt end of file (EOF or error).
                std::tie(line, errRead) = readLine();
                if (!line.empty()) {
                    pos.line++;
                    pos.col = 1;
                }
                if (errRead == io::EOF_) errRead = nullptr;
            } else {
                // Abrupt end of file (EOF or error).
                if (!LazyQuotes && !errRead) {
                    err = make_ptr<ParseError>(recLine, pos.line, pos.col, ErrQuote);
                    more = false;
                    break;
                }
                fieldIndexes_.push_back(recordBuffer_.size());
                fieldPositions_.push_back(fieldPos);
                more = false;
                break;
            }
        }
    }
    if (!err) err = errRead;

    // Create a single string and create views out of it, to batch
    // allocations.
    string str(recordBuffer_);
    dst.resize(fieldIndexes_.size());
    std::size_t preIdx = 0;
    for (std::size_t i = 0; i < fieldIndexes_.size(); i++) {
        dst[i] = str.slice(preIdx, fieldIndexes_[i]);
        preIdx = fieldIndexes_[i];
    }

    // Check or update the expected fields per record.
    if (FieldsPerRecord > 0) {
        if (dst.size() != std::size_t(FieldsPerRecord) && !err) {
            err = make_ptr<ParseError>(recLine, recLine, 1, ErrFieldCount);
        }
    } else if (FieldsPerRecord == 0) {
        FieldsPerRecord = int(dst.size());
    }
    return {std::move(dst), err};
}

// ---------------------------------------------------------------------------
// Writer

Writer::Writer(ptr<io::Writer> w) : w_(make_ptr<bufio::Writer>(std::move(w))) {}

ptr<Writer> NewWriter(ptr<io::Writer> w) { return make_ptr<Writer>(std::move(w)); }

error Writer::Write(const std::vector<string>& record) {
    if (!validDelim(Comma)) return errInvalidDelim;
    const std::string comma = encodeRune(Comma);

    for (std::size_t n = 0; n < record.size(); n++) {
        if (n > 0) {
            if (auto [_, err] = w_->WriteString(comma); err) return err;
        }

        // If we don't have to have a quoted field then just write out the
        // field and continue to the next field.
        std::string_view field = record[n];
        if (!fieldNeedsQuotes(field)) {
            if (auto [_, err] = w_->WriteString(field); err) return err;
            continue;
        }

        if (auto err = w_->WriteByte('"'); err) return err;
        while (!field.empty()) {
            // Search for special characters.
            std::size_t i = field.find_first_of("\"\r\n");
            if (i == field.npos) i = field.size();

            // Copy verbatim everything before the special character.
            if (auto [_, err] = w_->WriteString(field.substr(0, i)); err) return err;
            field = field.substr(i);

            // Encode the special character.
            if (!field.empty()) {
                error err;
                switch (field[0]) {
                    case '"':
                        err = w_->WriteString(R"("")").second;
                        break;
                    case '\r':
                        if (!UseCRLF) err = w_->WriteByte('\r');
                        break;
                    case '\n':
                        err = UseCRLF ? w_->WriteString("\r\n").second : w_->WriteByte('\n');
                        break;
                }
                field = field.substr(1);
                if (err) return err;
            }
        }
        if (auto err = w_->WriteByte('"'); err) return err;
    }
    return UseCRLF ? w_->WriteString("\r\n").second : w_->WriteByte('\n');
}

void Writer::Flush() { (void)w_->Flush(); }

error Writer::Error() { return w_->Write({}).second; }

error Writer::WriteAll(const std::vector<std::vector<string>>& records) {
    for (const auto& record : records) {
        if (auto err = Write(record); err) return err;
    }
    return w_->Flush();
}

// Whether field must be quoted: if it's \. (which Postgres reads as the
// end of data), contains Comma, a quote, \r or \n, or begins with a space.
bool Writer::fieldNeedsQuotes(std::string_view field) const {
    if (field.empty()) return false;
    if (field == R"(\.)") return true;
    if (Comma < utf8::RuneSelf) {
        for (char c : field) {
            if (c == '\n' || c == '\r' || c == '"' || c == char(Comma)) return true;
        }
    } else {
        if (field.find(encodeRune(Comma)) != field.npos || field.find_first_of("\"\r\n") != field.npos) return true;
    }
    return unicode::IsSpace(utf8::DecodeRuneInString(field).first);
}

}  // namespace gopp::csv
