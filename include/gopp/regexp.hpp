// The regexp package: regular expressions with RE2's syntax and guarantee
// of linear time in the input, ported from Go 1.27.3 (its NFA, backtracker
// and one-pass matchers).
//
//     auto re = regexp::MustCompile(`(\w+)@(\w+)\.com`);
//     re->MatchString("gopher@golang.com");                     // true
//     auto m = re->FindStringSubmatch("mail gopher@golang.com"); // {"gopher@golang.com", "gopher", "golang"}
//     re->ReplaceAllString("a@b.com", "$2 at $1");              // "b at a"
//
// The variants that return text take a slice<byte> or a string, and what
// they return shares its memory, as Go's results share their input's; the
// others take any std::span<const byte> or std::string_view.
// Indexes are std::vector<int>, empty where Go's are nil. A Regexp is safe
// for concurrent use, as Go's is.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/regexp/syntax.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::regexp {

// regexp.Regexp: a compiled regular expression.
class Regexp final : public object {
public:
    ~Regexp() override;

    // The source text of the expression.
    string String() const { return expr_; }
    // Makes future searches prefer leftmost-longest matches, as POSIX does,
    // instead of leftmost-first ones. Don't call it while the Regexp is in
    // use.
    void Longest() noexcept { longest_ = true; }
    // The number of parenthesized subexpressions.
    int NumSubexp() const noexcept { return numSubexp_; }
    // The names of the subexpressions, by index; "" for unnamed ones and
    // index 0, the whole expression.
    const std::vector<string>& SubexpNames() const noexcept { return subexpNames_; }
    // The index of the first subexpression named name, or -1.
    int SubexpIndex(std::string_view name) const;
    // The literal prefix every match starts with, and whether it is the
    // whole expression.
    std::pair<string, bool> LiteralPrefix() const { return {prefix_, prefixComplete_}; }

    // Whether b, s or r has a match.
    bool Match(std::span<const byte> b) const;
    bool MatchString(std::string_view s) const;
    bool MatchReader(io::RuneReader& r) const;

    // The leftmost match: its text (nil, "" for none), and its [start, end)
    // indexes (empty for none).
    slice<byte> Find(const slice<byte>& b) const;
    string FindString(const string& s) const;
    std::vector<int> FindIndex(std::span<const byte> b) const;
    std::vector<int> FindStringIndex(std::string_view s) const;
    std::vector<int> FindReaderIndex(io::RuneReader& r) const;

    // The leftmost match and its subexpressions' matches: the texts (nil,
    // "" for groups that didn't take part; empty for no match) and the
    // index pairs (-1 for groups that didn't take part).
    std::vector<slice<byte>> FindSubmatch(const slice<byte>& b) const;
    std::vector<string> FindStringSubmatch(const string& s) const;
    std::vector<int> FindSubmatchIndex(std::span<const byte> b) const;
    std::vector<int> FindStringSubmatchIndex(std::string_view s) const;
    std::vector<int> FindReaderSubmatchIndex(io::RuneReader& r) const;

    // The successive non-overlapping matches, at most n of them (all if n
    // is negative); empty for none. An empty match right after a match is
    // ignored.
    std::vector<slice<byte>> FindAll(const slice<byte>& b, int n) const;
    std::vector<string> FindAllString(const string& s, int n) const;
    std::vector<std::vector<int>> FindAllIndex(std::span<const byte> b, int n) const;
    std::vector<std::vector<int>> FindAllStringIndex(std::string_view s, int n) const;
    std::vector<std::vector<slice<byte>>> FindAllSubmatch(const slice<byte>& b, int n) const;
    std::vector<std::vector<string>> FindAllStringSubmatch(const string& s, int n) const;
    std::vector<std::vector<int>> FindAllSubmatchIndex(std::span<const byte> b, int n) const;
    std::vector<std::vector<int>> FindAllStringSubmatchIndex(std::string_view s, int n) const;

    // src with its matches replaced by repl, in which $1, ${name} and the
    // like are expanded (Expand); $$ is a $.
    string ReplaceAllString(const string& src, std::string_view repl) const;
    slice<byte> ReplaceAll(const slice<byte>& src, std::span<const byte> repl) const;
    // src with its matches replaced by repl as it is.
    string ReplaceAllLiteralString(const string& src, std::string_view repl) const;
    slice<byte> ReplaceAllLiteral(const slice<byte>& src, std::span<const byte> repl) const;
    // src with each match m replaced by repl(m).
    string ReplaceAllStringFunc(const string& src, const std::function<string(const string&)>& repl) const;
    slice<byte> ReplaceAllFunc(const slice<byte>& src, const std::function<slice<byte>(const slice<byte>&)>& repl) const;

    // Appends template to dst with $1, $name, ${1} and ${name} replaced by
    // the corresponding groups of match (FindSubmatchIndex's) in src: the
    // longest sequence of letters, digits and underscores after $ is the
    // name, so $1x is ${1x}. An unknown or unmatched group is empty; $$ is
    // a $.
    slice<byte> Expand(slice<byte> dst, std::span<const byte> tmpl, std::span<const byte> src,
                       const std::vector<int>& match) const;
    slice<byte> ExpandString(slice<byte> dst, std::string_view tmpl, std::string_view src,
                             const std::vector<int>& match) const;

    // s split around the matches: at most n parts, the last the unsplit
    // rest (all if n < 0); empty if n is 0.
    std::vector<string> Split(const string& s, int n) const;

    // MarshalText: the expression's text.
    std::pair<slice<byte>, error> MarshalText() const;

    struct impl;  // the compiled program, shared by copies

private:
    friend std::pair<ptr<Regexp>, error> compile(std::string_view expr, syntax::Flags mode, bool longest);
    Regexp() = default;

    std::vector<int> pad(std::vector<int> a) const;
    template <typename F>
    void matches(std::string_view s, int max, int ncap, F&& yield) const;
    std::string replaceAll(std::string_view src, int nmatch,
                           const std::function<void(std::string&, const std::vector<int>&)>& repl) const;
    void expand(std::string& dst, std::string_view tmpl, std::string_view src, const std::vector<int>& match) const;

    string expr_;
    std::shared_ptr<impl> impl_;
    int numSubexp_ = 0;
    std::vector<string> subexpNames_;
    string prefix_;
    bool prefixComplete_ = false;
    bool longest_ = false;
};

// regexp.Compile: parses expr (RE2's syntax, Perl-like) into a Regexp. A
// match is leftmost-first, as in Perl.
std::pair<ptr<Regexp>, error> Compile(std::string_view expr);
// regexp.CompilePOSIX: Compile with POSIX ERE syntax and leftmost-longest
// matches.
std::pair<ptr<Regexp>, error> CompilePOSIX(std::string_view expr);
// regexp.MustCompile: Compile, panicking on an error; for global
// expressions.
ptr<Regexp> MustCompile(std::string_view expr);
// regexp.MustCompilePOSIX
ptr<Regexp> MustCompilePOSIX(std::string_view expr);

// regexp.Match, MatchString and MatchReader: whether the input matches
// pattern, compiled for the occasion.
std::pair<bool, error> Match(std::string_view pattern, std::span<const byte> b);
std::pair<bool, error> MatchString(std::string_view pattern, std::string_view s);
std::pair<bool, error> MatchReader(std::string_view pattern, io::RuneReader& r);

// compile with the parser's flags, and leftmost-longest matching or not.
// Internal: Go's tests use it.
std::pair<ptr<Regexp>, error> compile(std::string_view expr, syntax::Flags mode, bool longest);

// regexp.QuoteMeta: s with the regular expression metacharacters escaped,
// a pattern matching s literally.
string QuoteMeta(const string& s);

}  // namespace gopp::regexp
