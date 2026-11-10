// The strings and strconv packages (the parts used so far).
#pragma once

#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <string>
#include <string_view>
#include <tuple>

namespace gopp::strings {

inline bool HasPrefix(std::string_view s, std::string_view prefix) noexcept { return s.starts_with(prefix); }
inline bool HasSuffix(std::string_view s, std::string_view suffix) noexcept { return s.ends_with(suffix); }
inline bool Contains(std::string_view s, std::string_view substr) noexcept { return s.find(substr) != s.npos; }
inline isize Index(std::string_view s, std::string_view substr) noexcept {
    auto i = s.find(substr);
    return i == s.npos ? -1 : static_cast<isize>(i);
}
inline isize IndexByte(std::string_view s, char c) noexcept {
    auto i = s.find(c);
    return i == s.npos ? -1 : static_cast<isize>(i);
}
inline isize LastIndex(std::string_view s, std::string_view substr) noexcept {
    auto i = s.rfind(substr);
    return i == s.npos ? -1 : static_cast<isize>(i);
}

// strings.EqualFold for ASCII.
bool EqualFold(std::string_view a, std::string_view b) noexcept;

// strings.Cut: the text before and after the first sep, and whether found.
std::tuple<string, string, bool> Cut(const string& s, std::string_view sep);

string TrimSpace(const string& s);
string Trim(const string& s, std::string_view cutset);
string TrimLeft(const string& s, std::string_view cutset);
string TrimRight(const string& s, std::string_view cutset);
string TrimPrefix(const string& s, std::string_view prefix);
string TrimSuffix(const string& s, std::string_view suffix);
string ToLower(std::string_view s);
string ToUpper(std::string_view s);
string Repeat(std::string_view s, std::size_t count);
slice<string> Split(const string& s, std::string_view sep);
string Join(std::span<const string> elems, std::string_view sep);

// strings.Builder
class Builder : public io::Writer {
public:
    std::pair<isize, error> Write(std::span<const byte> p) override {
        buf_.append(reinterpret_cast<const char*>(p.data()), p.size());
        return {static_cast<isize>(p.size()), nullptr};
    }
    void WriteString(std::string_view s) { buf_.append(s); }
    void WriteByte(char c) { buf_.push_back(c); }
    void Grow(std::size_t n) { buf_.reserve(buf_.size() + n); }
    std::size_t len() const noexcept { return buf_.size(); }
    void reset() noexcept { buf_.clear(); }
    std::string_view view() const noexcept { return buf_; }
    gopp::string str() const { return gopp::string(buf_); }

private:
    std::string buf_;
};

// strings.NewReader
inline ptr<io::StringReader> NewReader(string s) { return make_ptr<io::StringReader>(std::move(s)); }

}  // namespace gopp::strings

namespace gopp::strconv {

// The error of a failed conversion (strconv.NumError).
struct NumError : error_interface {
    string Func;
    string Num;
    gopp::error Err;  // error() hides the type name in this scope
    NumError(string f, string n, gopp::error e) : Func(std::move(f)), Num(std::move(n)), Err(std::move(e)) {}
    string Error() const override;
    const error_interface* Unwrap() const noexcept override { return Err.get(); }
};

extern const error ErrRange;
extern const error ErrSyntax;

string Itoa(int64 i);
string FormatInt(int64 i, int base);
string FormatUint(uint64 i, int base);
std::pair<isize, error> Atoi(std::string_view s);
std::pair<int64, error> ParseInt(std::string_view s, int base = 10, int bit_size = 64);
std::pair<uint64, error> ParseUint(std::string_view s, int base = 10, int bit_size = 64);
std::pair<bool, error> ParseBool(std::string_view s);
// strconv.Quote, for ASCII and UTF-8 text.
string Quote(std::string_view s);

}  // namespace gopp::strconv
