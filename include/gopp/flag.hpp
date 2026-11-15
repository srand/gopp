// The flag package: command-line flags, ported from Go 1.27.3.
//
//     auto* addr = flag::String("addr", "127.0.0.1:8080", "the `address` to listen on");
//     isize workers = 4;
//     flag::IntVar(&workers, "workers", workers, "number of workers");
//     flag::Parse();
//     ... *addr ...
//
// Flags are given as -name value, -name=value, or with two dashes; boolean
// flags as -name or -name=false; "--" or the first argument that isn't a
// flag ends them. Each type has the Var form, which binds a variable, and
// the plain one (String, Int, ...), which returns a pointer to a variable
// the flag owns, valid as long as its FlagSet. Go's int is isize here, and
// Go's uint std::size_t. As in Go, defining a flag twice panics.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <any>
#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::flag {

// flag.ErrHelp: -h or -help was given without such a flag defined.
extern const gopp::error ErrHelp;

// flag.Value: the value of a flag, which parsing Sets from text. Custom
// flag types implement it, and are defined with Var. PrintDefaults shows a
// flag's default unless it is the String of a default-constructed value of
// the flag's type.
struct Value : public virtual object {
    virtual gopp::string String() const = 0;
    virtual gopp::error Set(gopp::string s) = 0;
};

// A Value whose flag needs no argument (Go's IsBoolFlag method): -name
// means -name=true.
struct BoolFlag : public virtual Value {
    virtual bool IsBoolFlag() const = 0;
};

// flag.Getter: a Value whose contents can be retrieved; the Values of the
// flags this package defines all are (Get's type is the flag's: bool,
// isize, gopp::string, ...).
struct Getter : public virtual Value {
    virtual std::any Get() const = 0;
};

// flag.ErrorHandling: what Parse does on an error.
enum ErrorHandling {
    ContinueOnError,  // return the error
    ExitOnError,      // exit with status 2, or 0 for -h and -help
    PanicOnError,     // panic with the error
};

// flag.Flag: the state of a flag.
struct Flag {
    gopp::string Name;          // as on the command line
    gopp::string Usage;         // the help message
    ptr<flag::Value> Value;     // the value as set
    gopp::string DefValue;      // the default, as text, for the usage message
    // The String of a zero Value of the flag's type, or nil if that can't be
    // made; it may throw. For PrintDefaults.
    std::function<gopp::string()> ZeroString;
};

// The type of text flags (TextVar): Go's encoding.TextMarshaler and
// TextUnmarshaler.
template <typename T>
concept TextValue = requires(T& t, const T& c, std::span<const byte> b) {
    { c.MarshalText() } -> std::convertible_to<std::pair<slice<byte>, gopp::error>>;
    { t.UnmarshalText(b) } -> std::convertible_to<gopp::error>;
};

namespace detail {
// Go's ResetForTesting: a new CommandLine that returns its errors and
// writes nothing, and usage as Usage.
void resetForTesting(std::function<void()> usage);
// Go's DefaultUsage: Usage as the package defines it.
void defaultUsage();
ptr<Getter> newTextValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set,
                         std::function<std::any()> get);
}  // namespace detail

// flag.FlagSet: a set of flags.
class FlagSet final : public object {
public:
    // Called when parsing fails; prints the flags to Output() by default.
    std::function<void()> Usage;

    // flag.NewFlagSet
    explicit FlagSet(gopp::string name, flag::ErrorHandling errorHandling = ContinueOnError);
    FlagSet(const FlagSet&) = delete;
    FlagSet& operator=(const FlagSet&) = delete;

    // Sets the name and the error handling.
    void Init(gopp::string name, flag::ErrorHandling errorHandling);
    const gopp::string& Name() const noexcept { return name_; }
    flag::ErrorHandling ErrorHandling() const noexcept { return errorHandling_; }

    // Where usage and error messages go; standard error if nil.
    io::Writer& Output() const;
    void SetOutput(ptr<io::Writer> output) { output_ = std::move(output); }

    // Calls fn for each flag, in lexical order.
    void VisitAll(const std::function<void(Flag*)>& fn) const;
    // Calls fn for each flag that has been set, in lexical order.
    void Visit(const std::function<void(Flag*)>& fn) const;
    // The flag named name, or nullptr.
    Flag* Lookup(std::string_view name) const;
    // Sets the named flag, as parsing would.
    gopp::error Set(std::string_view name, std::string_view value,
                    std::source_location loc = std::source_location::current());

    // Prints the flags and their defaults to Output(), in Go's format.
    void PrintDefaults() const;
    // The number of flags that have been set.
    isize NFlag() const noexcept { return static_cast<isize>(actual_.size()); }
    // The i'th argument after the flags; "" if there is none.
    gopp::string Arg(isize i) const;
    isize NArg() const noexcept { return static_cast<isize>(args_.size()); }
    // The arguments after the flags.
    const std::vector<gopp::string>& Args() const noexcept { return args_; }

    void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
    bool* Bool(std::string_view name, bool value, std::string_view usage);
    void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
    isize* Int(std::string_view name, isize value, std::string_view usage);
    void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
    int64* Int64(std::string_view name, int64 value, std::string_view usage);
    void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage);
    std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage);
    void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage);
    uint64* Uint64(std::string_view name, uint64 value, std::string_view usage);
    void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
    gopp::string* String(std::string_view name, gopp::string value, std::string_view usage);
    void Float64Var(double* p, std::string_view name, double value, std::string_view usage);
    double* Float64(std::string_view name, double value, std::string_view usage);
    void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);
    time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage);

    // TextVar: a flag of a type with MarshalText and UnmarshalText, such
    // as netip::Addr; p starts as value.
    template <TextValue T>
    void TextVar(T* p, std::string_view name, T value, std::string_view usage) {
        *p = std::move(value);
        Var(detail::newTextValue(
                [p] {
                    auto [b, err] = p->MarshalText();
                    return err ? gopp::string() : gopp::String(b);
                },
                [p](gopp::string s) { return p->UnmarshalText(as_bytes(s.view())); }, [p] { return std::any(p); }),
            name, usage);
    }
    // Func: a flag that calls fn with each value it's given.
    void Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
    // BoolFunc: likewise, a flag that needs no value ("true" if none).
    void BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);

    // Var: a flag of a custom Value type. Panics if the name begins with
    // "-", contains "=", or is defined already.
    void Var(ptr<flag::Value> value, std::string_view name, std::string_view usage) { define(std::move(value), name, usage, nullptr); }
    template <typename V>
        requires std::derived_from<V, flag::Value>
    void Var(ptr<V> value, std::string_view name, std::string_view usage) {
        std::function<gopp::string()> zero;
        if constexpr (std::is_default_constructible_v<V>) zero = [] { return make_ptr<V>()->String(); };
        define(std::move(value), name, usage, std::move(zero));
    }

    // Parses the arguments, without the program name. Must be called after
    // the flags are defined and before they're used.
    gopp::error Parse(std::span<const gopp::string> arguments);
    bool Parsed() const noexcept { return parsed_; }

private:
    friend FlagSet& CommandLine();
    void define(ptr<flag::Value> value, std::string_view name, std::string_view usage,
                std::function<gopp::string()> zero);
    std::pair<bool, gopp::error> parseOne();
    gopp::error failf(gopp::string msg);
    gopp::string sprintln(gopp::string msg) const;
    void usage();
    void defaultUsage();

    gopp::string name_;
    bool parsed_ = false;
    std::map<gopp::string, std::unique_ptr<Flag>, std::less<>> formal_;
    std::map<gopp::string, Flag*, std::less<>> actual_;
    std::vector<gopp::string> args_;  // the arguments after the flags
    flag::ErrorHandling errorHandling_;
    ptr<io::Writer> output_;
    std::map<gopp::string, gopp::string, std::less<>> undef_;  // set before being defined: where
};

// flag.NewFlagSet
ptr<FlagSet> NewFlagSet(gopp::string name, ErrorHandling errorHandling);

// flag.CommandLine: the program's flags, named after the program, which
// the functions below define and parse. It exits on errors.
FlagSet& CommandLine();

// flag.Usage: prints the command line's usage to its Output(), as
// CommandLine().Usage does unless replaced.
extern std::function<void()> Usage;

// flag.UnquoteUsage: the name of a flag's value in its usage message (the
// first `quoted` word, or the type: "int", "string", ...; "" for a
// boolean), and the usage without the back quotes.
std::pair<gopp::string, gopp::string> UnquoteUsage(const Flag& flag);

// The CommandLine's.
void VisitAll(const std::function<void(Flag*)>& fn);
void Visit(const std::function<void(Flag*)>& fn);
Flag* Lookup(std::string_view name);
gopp::error Set(std::string_view name, std::string_view value, std::source_location loc = std::source_location::current());
void PrintDefaults();
isize NFlag();
gopp::string Arg(isize i);
isize NArg();
const std::vector<gopp::string>& Args();

void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
bool* Bool(std::string_view name, bool value, std::string_view usage);
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
isize* Int(std::string_view name, isize value, std::string_view usage);
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
int64* Int64(std::string_view name, int64 value, std::string_view usage);
void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage);
std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage);
void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage);
uint64* Uint64(std::string_view name, uint64 value, std::string_view usage);
void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
gopp::string* String(std::string_view name, gopp::string value, std::string_view usage);
void Float64Var(double* p, std::string_view name, double value, std::string_view usage);
double* Float64(std::string_view name, double value, std::string_view usage);
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);
time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage);
template <TextValue T>
void TextVar(T* p, std::string_view name, T value, std::string_view usage) {
    CommandLine().TextVar(p, name, std::move(value), usage);
}
void Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
void BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
inline void Var(ptr<flag::Value> value, std::string_view name, std::string_view usage) {
    CommandLine().Var(std::move(value), name, usage);
}
template <typename V>
    requires std::derived_from<V, flag::Value>
void Var(ptr<V> value, std::string_view name, std::string_view usage) {
    CommandLine().Var(std::move(value), name, usage);
}

// flag.Parse: parses the program's arguments (os::Args() but the first)
// into the CommandLine's flags.
void Parse();
// The same, with os::Args() set from argv first.
void Parse(int argc, char** argv);
bool Parsed();

}  // namespace gopp::flag
