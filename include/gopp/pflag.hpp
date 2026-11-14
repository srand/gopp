// The pflag package: POSIX/GNU-style command-line flags, ported from
// spf13/pflag v1.0.10, the flags of cobra (gopp::cmd) and kubectl.
//
//     auto fs = pflag::NewFlagSet("app", pflag::ContinueOnError);
//     string* addr = fs->StringP("addr", "a", "127.0.0.1:8080", "server address");
//     isize verbosity = 0;
//     fs->CountVarP(&verbosity, "verbose", "v", "more output (repeatable)");
//     std::vector<string> tags;
//     fs->StringSliceVar(&tags, "tag", {}, "tags, comma-separated");
//     if (auto err = fs->Parse(std::span(os::Args()).subspan(1))) { ... }
//
// Unlike the flag package, single dashes are for one-letter shorthands,
// which combine (-abc, -n1234, -n=1234, -n 1234), and long flags take two
// (--flag, --flag=x, --flag x). Flags and arguments may be interspersed
// until "--". Boolean flags take no value unless given with =.
//
// Each flag type has the four Go forms, on a FlagSet and for CommandLine():
// XVar binds a variable, XVarP the same with a shorthand, and X and XP
// return a pointer to a variable the flag owns (valid while a FlagSet holds
// the flag). GetX reads a flag's value back by name. Go's int is isize here,
// and Go's uint std::size_t. Not ported: AddGoFlagSet and the other bridges
// to Go's flag package.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <concepts>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace gopp::pflag {

// pflag.ErrHelp: -h or --help was given without such a flag defined.
extern const gopp::error ErrHelp;

// pflag.ErrorHandling: what Parse does on an error.
enum ErrorHandling {
    ContinueOnError,  // return the error
    ExitOnError,      // exit with status 2 (0 for ErrHelp)
    PanicOnError,     // panic with the error
};

// pflag.ParseErrorsAllowlist: errors Parse ignores.
struct ParseErrorsAllowlist {
    // Ignore unknown flags (and the value after one) and go on parsing.
    bool UnknownFlags = false;
};
// The deprecated name.
using ParseErrorsWhitelist = ParseErrorsAllowlist;

// pflag.NormalizedName: a flag name as a FlagSet's normalization function
// makes it.
using NormalizedName = gopp::string;

// pflag.Value: the value of a flag, which parsing Sets from text.
// Custom flag types implement it, and are defined with FlagSet::Var.
struct Value : public virtual object {
    virtual gopp::string String() const = 0;
    virtual gopp::error Set(gopp::string s) = 0;
    // The type's name, shown in usage messages ("string", "int", ...).
    virtual gopp::string Type() const = 0;
};

// pflag.SliceValue: a Value holding a list.
struct SliceValue : public virtual object {
    // Appends one element.
    virtual gopp::error Append(gopp::string s) = 0;
    // Replaces the elements.
    virtual gopp::error Replace(std::vector<gopp::string> s) = 0;
    // The elements as text.
    virtual std::vector<gopp::string> GetSlice() const = 0;
};

class FlagSet;

// pflag.Flag: a flag's definition and state. FlagSets share flags (cobra
// adds a command's persistent flags to its children's sets).
class Flag final : public object {
public:
    gopp::string Name;       // as it appears on the command line, after --
    gopp::string Shorthand;  // the one-letter abbreviation, after -
    gopp::string Usage;      // the help message
    ptr<pflag::Value> Value;
    gopp::string DefValue;   // the default value as text, for the usage message
    bool Changed = false;    // whether the command line set it
    // The value when the flag is given without one (--flag): "true" for
    // boolean flags, "+1" for counts.
    gopp::string NoOptDefVal;
    // If set, the flag is deprecated (and hidden), and this is shown when
    // it's used.
    gopp::string Deprecated;
    bool Hidden = false;  // not shown in the usage message
    // If set, the shorthand is deprecated, and this is shown when it's used.
    gopp::string ShorthandDeprecated;
    // Annotations, which cobra uses for completion and flag groups.
    std::map<gopp::string, std::vector<gopp::string>> Annotations;
};

// pflag.UnquoteUsage: the name of a flag's value, from back quotes in its
// usage (`file`) or from its type, and the usage without the quotes.
std::pair<gopp::string, gopp::string> UnquoteUsage(const Flag* flag);

// pflag.ParseIPv4Mask: a mask in dotted (255.255.255.0) or hex (ffffff00)
// notation; nil if s is neither.
net::IPMask ParseIPv4Mask(std::string_view s);

// The type of text flags (TextVar): Go's encoding.TextMarshaler and
// TextUnmarshaler.
template <typename T>
concept TextValue = requires(T& t, const T& c, std::span<const byte> b) {
    { c.MarshalText() } -> std::convertible_to<std::pair<slice<byte>, gopp::error>>;
    { t.UnmarshalText(b) } -> std::convertible_to<gopp::error>;
};

namespace detail {
ptr<Value> newTextValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set);
}  // namespace detail

// pflag.FlagSet: a set of flags. Commands each have a few.
class FlagSet final : public object {
public:
    // Called on parse errors; nil means print the flags to Output().
    std::function<void()> Usage;
    // Whether usage messages and VisitAll sort flags by name, or keep them
    // in definition order.
    bool SortFlags = true;
    // The errors Parse ignores.
    pflag::ParseErrorsAllowlist ParseErrorsAllowlist;
    // The deprecated name of ParseErrorsAllowlist; either applies.
    pflag::ParseErrorsAllowlist ParseErrorsWhitelist;

    // pflag.NewFlagSet
    FlagSet(gopp::string name, ErrorHandling errorHandling);
    ~FlagSet() override;

    // Sets the name and error handling.
    void Init(gopp::string name, ErrorHandling errorHandling);
    const gopp::string& Name() const noexcept { return name_; }

    // Normalization of flag names, applied when they are defined and looked
    // up: say, so that --my_flag and --my-flag are the same flag.
    using NormalizeFunc = std::function<NormalizedName(FlagSet* f, std::string_view name)>;
    void SetNormalizeFunc(NormalizeFunc n);
    // The normalization function; the identity if none is set.
    NormalizeFunc GetNormalizeFunc() const;

    // Where usage and error messages go; standard error if nil.
    io::Writer& Output() const;
    void SetOutput(ptr<io::Writer> output);

    // Calls fn for each flag: sorted by name if SortFlags, else in the
    // order of definition.
    void VisitAll(const std::function<void(Flag*)>& fn) const;
    // Calls fn for each flag that was set.
    void Visit(const std::function<void(Flag*)>& fn) const;
    bool HasFlags() const noexcept;
    // Whether any flag isn't hidden.
    bool HasAvailableFlags() const noexcept;

    // The flag named name, or nullptr.
    Flag* Lookup(std::string_view name) const;
    // The flag with shorthand name (one ASCII character), or nullptr.
    Flag* ShorthandLookup(std::string_view name) const;

    // The number of arguments before "--", or -1 if there was none.
    isize ArgsLenAtDash() const noexcept { return argsLenAtDash_; }

    // Marks a flag deprecated, which hides it; usageMessage is shown when
    // it's used.
    gopp::error MarkDeprecated(std::string_view name, std::string_view usageMessage);
    // Marks a flag's shorthand deprecated.
    gopp::error MarkShorthandDeprecated(std::string_view name, std::string_view usageMessage);
    // Hides a flag from usage messages.
    gopp::error MarkHidden(std::string_view name);

    // Sets the named flag's value, as the command line would.
    gopp::error Set(std::string_view name, gopp::string value);
    // Sets an annotation of the named flag.
    gopp::error SetAnnotation(std::string_view name, std::string_view key, std::vector<gopp::string> values);
    // Whether the named flag was set.
    bool Changed(std::string_view name) const;

    // Prints FlagUsages() to Output().
    void PrintDefaults() const;
    // The flags' usage lines, wrapped to cols columns (0: not wrapped).
    gopp::string FlagUsagesWrapped(int cols) const;
    gopp::string FlagUsages() const { return FlagUsagesWrapped(0); }

    // The number of flags set.
    isize NFlag() const noexcept { return static_cast<isize>(actual_.size()); }
    // Argument i after the flags; "" if there's none.
    gopp::string Arg(isize i) const;
    isize NArg() const noexcept { return static_cast<isize>(args_.size()); }
    // The arguments after the flags.
    const std::vector<gopp::string>& Args() const noexcept { return args_; }

    // Defines a flag of a custom type.
    void Var(ptr<pflag::Value> value, std::string_view name, std::string_view usage);
    void VarP(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage);
    // VarP, returning the flag (to set NoOptDefVal, say).
    Flag* VarPF(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage);
    // Adds a flag, which may be another FlagSet's. Panics if its name or
    // shorthand is defined already.
    void AddFlag(Flag* flag);
    // Adds newSet's flags that aren't defined here.
    void AddFlagSet(const FlagSet* newSet);

    // Parses the arguments, without the program's name. On errors, acts
    // as the error handling says.
    gopp::error Parse(std::span<const gopp::string> arguments);
    // Parse, calling fn for each flag and value instead of setting them.
    gopp::error ParseAll(std::span<const gopp::string> arguments, const std::function<gopp::error(Flag*, gopp::string)>& fn);
    bool Parsed() const noexcept { return parsed_; }
    // Whether arguments may come before flags (the default), or the
    // first one ends the flags.
    void SetInterspersed(bool interspersed) noexcept { interspersed_ = interspersed; }

    // Count: an int flag counting how often it's given (-vvv is 3); a value
    // sets it (--verbose=2).
    void CountVar(isize* p, std::string_view name, std::string_view usage);
    void CountVarP(isize* p, std::string_view name, std::string_view shorthand, std::string_view usage);
    isize* Count(std::string_view name, std::string_view usage);
    isize* CountP(std::string_view name, std::string_view shorthand, std::string_view usage);
    std::pair<isize, gopp::error> GetCount(std::string_view name) const;

    // Time: a time::Time flag, parsed with the first of formats that fits
    // (time::RFC3339, say).
    void TimeVar(time::Time* p, std::string_view name, time::Time value, std::vector<gopp::string> formats, std::string_view usage);
    void TimeVarP(time::Time* p, std::string_view name, std::string_view shorthand, time::Time value,
                  std::vector<gopp::string> formats, std::string_view usage);
    time::Time* Time(std::string_view name, time::Time value, std::vector<gopp::string> formats, std::string_view usage);
    time::Time* TimeP(std::string_view name, std::string_view shorthand, time::Time value, std::vector<gopp::string> formats,
                      std::string_view usage);
    std::pair<time::Time, gopp::error> GetTime(std::string_view name) const;

    // Func: a flag that calls fn with each value given.
    void Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
    void FuncP(std::string_view name, std::string_view shorthand, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
    // BoolFunc: Func for a flag that takes no value (fn gets "true").
    void BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
    void BoolFuncP(std::string_view name, std::string_view shorthand, std::string_view usage, std::function<gopp::error(gopp::string)> fn);

    // TextVar: a flag of a type with MarshalText and UnmarshalText, such
    // as netip::Addr; p starts as value.
    template <TextValue T>
    void TextVar(T* p, std::string_view name, T value, std::string_view usage) {
        TextVarP(p, name, {}, std::move(value), usage);
    }
    template <TextValue T>
    void TextVarP(T* p, std::string_view name, std::string_view shorthand, T value, std::string_view usage) {
        *p = std::move(value);
        VarP(detail::newTextValue(
                 [p] {
                     auto [b, err] = p->MarshalText();
                     return err ? gopp::string() : gopp::String(b);
                 },
                 [p](gopp::string s) { return p->UnmarshalText(as_bytes(s.view())); }),
             name, shorthand, usage);
    }
    // Reads a text flag's value into out.
    template <TextValue T>
    gopp::error GetText(std::string_view name, T* out) const {
        Flag* flag = Lookup(name);
        if (!flag) return notDefined(name);
        // Go compares the type names, which are empty for text flags.
        if (!flag->Value->Type().empty()) return wrongType("", flag->Value->Type());
        return out->UnmarshalText(as_bytes(flag->Value->String().view()));
    }

    // The flags of each type (a block per type, as in pflag's files).
    // Bool: a bool flag; --name alone means true.
    void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
    void BoolVarP(bool* p, std::string_view name, std::string_view shorthand, bool value, std::string_view usage);
    bool* Bool(std::string_view name, bool value, std::string_view usage);
    bool* BoolP(std::string_view name, std::string_view shorthand, bool value, std::string_view usage);
    std::pair<bool, gopp::error> GetBool(std::string_view name) const;

    // String: a string flag.
    void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
    void StringVarP(gopp::string* p, std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage);
    gopp::string* String(std::string_view name, gopp::string value, std::string_view usage);
    gopp::string* StringP(std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage);
    std::pair<gopp::string, gopp::error> GetString(std::string_view name) const;

    // Int: an int flag (Go's int, isize here).
    void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
    void IntVarP(isize* p, std::string_view name, std::string_view shorthand, isize value, std::string_view usage);
    isize* Int(std::string_view name, isize value, std::string_view usage);
    isize* IntP(std::string_view name, std::string_view shorthand, isize value, std::string_view usage);
    std::pair<isize, gopp::error> GetInt(std::string_view name) const;

    // Int8: an int8 flag.
    void Int8Var(int8* p, std::string_view name, int8 value, std::string_view usage);
    void Int8VarP(int8* p, std::string_view name, std::string_view shorthand, int8 value, std::string_view usage);
    int8* Int8(std::string_view name, int8 value, std::string_view usage);
    int8* Int8P(std::string_view name, std::string_view shorthand, int8 value, std::string_view usage);
    std::pair<int8, gopp::error> GetInt8(std::string_view name) const;

    // Int16: an int16 flag.
    void Int16Var(int16* p, std::string_view name, int16 value, std::string_view usage);
    void Int16VarP(int16* p, std::string_view name, std::string_view shorthand, int16 value, std::string_view usage);
    int16* Int16(std::string_view name, int16 value, std::string_view usage);
    int16* Int16P(std::string_view name, std::string_view shorthand, int16 value, std::string_view usage);
    std::pair<int16, gopp::error> GetInt16(std::string_view name) const;

    // Int32: an int32 flag.
    void Int32Var(int32* p, std::string_view name, int32 value, std::string_view usage);
    void Int32VarP(int32* p, std::string_view name, std::string_view shorthand, int32 value, std::string_view usage);
    int32* Int32(std::string_view name, int32 value, std::string_view usage);
    int32* Int32P(std::string_view name, std::string_view shorthand, int32 value, std::string_view usage);
    std::pair<int32, gopp::error> GetInt32(std::string_view name) const;

    // Int64: an int64 flag.
    void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
    void Int64VarP(int64* p, std::string_view name, std::string_view shorthand, int64 value, std::string_view usage);
    int64* Int64(std::string_view name, int64 value, std::string_view usage);
    int64* Int64P(std::string_view name, std::string_view shorthand, int64 value, std::string_view usage);
    std::pair<int64, gopp::error> GetInt64(std::string_view name) const;

    // Uint: a uint flag (Go's uint, std::size_t here).
    void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage);
    void UintVarP(std::size_t* p, std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage);
    std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage);
    std::size_t* UintP(std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage);
    std::pair<std::size_t, gopp::error> GetUint(std::string_view name) const;

    // Uint8: a uint8 flag.
    void Uint8Var(uint8* p, std::string_view name, uint8 value, std::string_view usage);
    void Uint8VarP(uint8* p, std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage);
    uint8* Uint8(std::string_view name, uint8 value, std::string_view usage);
    uint8* Uint8P(std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage);
    std::pair<uint8, gopp::error> GetUint8(std::string_view name) const;

    // Uint16: a uint16 flag.
    void Uint16Var(uint16* p, std::string_view name, uint16 value, std::string_view usage);
    void Uint16VarP(uint16* p, std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage);
    uint16* Uint16(std::string_view name, uint16 value, std::string_view usage);
    uint16* Uint16P(std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage);
    std::pair<uint16, gopp::error> GetUint16(std::string_view name) const;

    // Uint32: a uint32 flag.
    void Uint32Var(uint32* p, std::string_view name, uint32 value, std::string_view usage);
    void Uint32VarP(uint32* p, std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage);
    uint32* Uint32(std::string_view name, uint32 value, std::string_view usage);
    uint32* Uint32P(std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage);
    std::pair<uint32, gopp::error> GetUint32(std::string_view name) const;

    // Uint64: a uint64 flag.
    void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage);
    void Uint64VarP(uint64* p, std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage);
    uint64* Uint64(std::string_view name, uint64 value, std::string_view usage);
    uint64* Uint64P(std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage);
    std::pair<uint64, gopp::error> GetUint64(std::string_view name) const;

    // Float32: a float32 flag.
    void Float32Var(float* p, std::string_view name, float value, std::string_view usage);
    void Float32VarP(float* p, std::string_view name, std::string_view shorthand, float value, std::string_view usage);
    float* Float32(std::string_view name, float value, std::string_view usage);
    float* Float32P(std::string_view name, std::string_view shorthand, float value, std::string_view usage);
    std::pair<float, gopp::error> GetFloat32(std::string_view name) const;

    // Float64: a float64 flag.
    void Float64Var(double* p, std::string_view name, double value, std::string_view usage);
    void Float64VarP(double* p, std::string_view name, std::string_view shorthand, double value, std::string_view usage);
    double* Float64(std::string_view name, double value, std::string_view usage);
    double* Float64P(std::string_view name, std::string_view shorthand, double value, std::string_view usage);
    std::pair<double, gopp::error> GetFloat64(std::string_view name) const;

    // Duration: a time::Duration flag, in time::ParseDuration's syntax.
    void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);
    void DurationVarP(time::Duration* p, std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage);
    time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage);
    time::Duration* DurationP(std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage);
    std::pair<time::Duration, gopp::error> GetDuration(std::string_view name) const;

    // BytesHex: a []byte flag given in hex.
    void BytesHexVar(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage);
    void BytesHexVarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
    slice<byte>* BytesHex(std::string_view name, slice<byte> value, std::string_view usage);
    slice<byte>* BytesHexP(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
    std::pair<slice<byte>, gopp::error> GetBytesHex(std::string_view name) const;

    // BytesBase64: a []byte flag given in standard base64.
    void BytesBase64Var(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage);
    void BytesBase64VarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
    slice<byte>* BytesBase64(std::string_view name, slice<byte> value, std::string_view usage);
    slice<byte>* BytesBase64P(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
    std::pair<slice<byte>, gopp::error> GetBytesBase64(std::string_view name) const;

    // IP: a net::IP flag.
    void IPVar(net::IP* p, std::string_view name, net::IP value, std::string_view usage);
    void IPVarP(net::IP* p, std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage);
    net::IP* IP(std::string_view name, net::IP value, std::string_view usage);
    net::IP* IPP(std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage);
    std::pair<net::IP, gopp::error> GetIP(std::string_view name) const;

    // IPMask: a net::IPMask flag, as 255.255.255.0 or ffffff00.
    void IPMaskVar(net::IPMask* p, std::string_view name, net::IPMask value, std::string_view usage);
    void IPMaskVarP(net::IPMask* p, std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage);
    net::IPMask* IPMask(std::string_view name, net::IPMask value, std::string_view usage);
    net::IPMask* IPMaskP(std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage);
    std::pair<net::IPMask, gopp::error> GetIPv4Mask(std::string_view name) const;

    // IPNet: a net::IPNet flag, in CIDR notation.
    void IPNetVar(net::IPNet* p, std::string_view name, net::IPNet value, std::string_view usage);
    void IPNetVarP(net::IPNet* p, std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage);
    net::IPNet* IPNet(std::string_view name, net::IPNet value, std::string_view usage);
    net::IPNet* IPNetP(std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage);
    std::pair<net::IPNet, gopp::error> GetIPNet(std::string_view name) const;

    // StringSlice: a []string flag: comma-separated (CSV) values, which repeated flags append to.
    void StringSliceVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage);
    void StringSliceVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
    std::vector<gopp::string>* StringSlice(std::string_view name, std::vector<gopp::string> value, std::string_view usage);
    std::vector<gopp::string>* StringSliceP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
    std::pair<std::vector<gopp::string>, gopp::error> GetStringSlice(std::string_view name) const;

    // StringArray: a []string flag: each flag one value, commas and all.
    void StringArrayVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage);
    void StringArrayVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
    std::vector<gopp::string>* StringArray(std::string_view name, std::vector<gopp::string> value, std::string_view usage);
    std::vector<gopp::string>* StringArrayP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
    std::pair<std::vector<gopp::string>, gopp::error> GetStringArray(std::string_view name) const;

    // IntSlice: an []int flag: comma-separated, repeated flags append.
    void IntSliceVar(std::vector<isize>* p, std::string_view name, std::vector<isize> value, std::string_view usage);
    void IntSliceVarP(std::vector<isize>* p, std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage);
    std::vector<isize>* IntSlice(std::string_view name, std::vector<isize> value, std::string_view usage);
    std::vector<isize>* IntSliceP(std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage);
    std::pair<std::vector<isize>, gopp::error> GetIntSlice(std::string_view name) const;

    // Int32Slice: an []int32 flag.
    void Int32SliceVar(std::vector<int32>* p, std::string_view name, std::vector<int32> value, std::string_view usage);
    void Int32SliceVarP(std::vector<int32>* p, std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage);
    std::vector<int32>* Int32Slice(std::string_view name, std::vector<int32> value, std::string_view usage);
    std::vector<int32>* Int32SliceP(std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage);
    std::pair<std::vector<int32>, gopp::error> GetInt32Slice(std::string_view name) const;

    // Int64Slice: an []int64 flag.
    void Int64SliceVar(std::vector<int64>* p, std::string_view name, std::vector<int64> value, std::string_view usage);
    void Int64SliceVarP(std::vector<int64>* p, std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage);
    std::vector<int64>* Int64Slice(std::string_view name, std::vector<int64> value, std::string_view usage);
    std::vector<int64>* Int64SliceP(std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage);
    std::pair<std::vector<int64>, gopp::error> GetInt64Slice(std::string_view name) const;

    // UintSlice: a []uint flag.
    void UintSliceVar(std::vector<std::size_t>* p, std::string_view name, std::vector<std::size_t> value, std::string_view usage);
    void UintSliceVarP(std::vector<std::size_t>* p, std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage);
    std::vector<std::size_t>* UintSlice(std::string_view name, std::vector<std::size_t> value, std::string_view usage);
    std::vector<std::size_t>* UintSliceP(std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage);
    std::pair<std::vector<std::size_t>, gopp::error> GetUintSlice(std::string_view name) const;

    // Float32Slice: a []float32 flag.
    void Float32SliceVar(std::vector<float>* p, std::string_view name, std::vector<float> value, std::string_view usage);
    void Float32SliceVarP(std::vector<float>* p, std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage);
    std::vector<float>* Float32Slice(std::string_view name, std::vector<float> value, std::string_view usage);
    std::vector<float>* Float32SliceP(std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage);
    std::pair<std::vector<float>, gopp::error> GetFloat32Slice(std::string_view name) const;

    // Float64Slice: a []float64 flag.
    void Float64SliceVar(std::vector<double>* p, std::string_view name, std::vector<double> value, std::string_view usage);
    void Float64SliceVarP(std::vector<double>* p, std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage);
    std::vector<double>* Float64Slice(std::string_view name, std::vector<double> value, std::string_view usage);
    std::vector<double>* Float64SliceP(std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage);
    std::pair<std::vector<double>, gopp::error> GetFloat64Slice(std::string_view name) const;

    // BoolSlice: a []bool flag.
    void BoolSliceVar(std::vector<bool>* p, std::string_view name, std::vector<bool> value, std::string_view usage);
    void BoolSliceVarP(std::vector<bool>* p, std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage);
    std::vector<bool>* BoolSlice(std::string_view name, std::vector<bool> value, std::string_view usage);
    std::vector<bool>* BoolSliceP(std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage);
    std::pair<std::vector<bool>, gopp::error> GetBoolSlice(std::string_view name) const;

    // DurationSlice: a []time::Duration flag.
    void DurationSliceVar(std::vector<time::Duration>* p, std::string_view name, std::vector<time::Duration> value, std::string_view usage);
    void DurationSliceVarP(std::vector<time::Duration>* p, std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage);
    std::vector<time::Duration>* DurationSlice(std::string_view name, std::vector<time::Duration> value, std::string_view usage);
    std::vector<time::Duration>* DurationSliceP(std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage);
    std::pair<std::vector<time::Duration>, gopp::error> GetDurationSlice(std::string_view name) const;

    // IPSlice: a []net::IP flag.
    void IPSliceVar(std::vector<net::IP>* p, std::string_view name, std::vector<net::IP> value, std::string_view usage);
    void IPSliceVarP(std::vector<net::IP>* p, std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage);
    std::vector<net::IP>* IPSlice(std::string_view name, std::vector<net::IP> value, std::string_view usage);
    std::vector<net::IP>* IPSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage);
    std::pair<std::vector<net::IP>, gopp::error> GetIPSlice(std::string_view name) const;

    // IPNetSlice: a []net::IPNet flag.
    void IPNetSliceVar(std::vector<net::IPNet>* p, std::string_view name, std::vector<net::IPNet> value, std::string_view usage);
    void IPNetSliceVarP(std::vector<net::IPNet>* p, std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage);
    std::vector<net::IPNet>* IPNetSlice(std::string_view name, std::vector<net::IPNet> value, std::string_view usage);
    std::vector<net::IPNet>* IPNetSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage);
    std::pair<std::vector<net::IPNet>, gopp::error> GetIPNetSlice(std::string_view name) const;

    // StringToString: a map[string]string flag: key=value pairs, comma-separated.
    void StringToStringVar(std::map<gopp::string, gopp::string>* p, std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage);
    void StringToStringVarP(std::map<gopp::string, gopp::string>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage);
    std::map<gopp::string, gopp::string>* StringToString(std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage);
    std::map<gopp::string, gopp::string>* StringToStringP(std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage);
    std::pair<std::map<gopp::string, gopp::string>, gopp::error> GetStringToString(std::string_view name) const;

    // StringToInt: a map[string]int flag.
    void StringToIntVar(std::map<gopp::string, isize>* p, std::string_view name, std::map<gopp::string, isize> value, std::string_view usage);
    void StringToIntVarP(std::map<gopp::string, isize>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage);
    std::map<gopp::string, isize>* StringToInt(std::string_view name, std::map<gopp::string, isize> value, std::string_view usage);
    std::map<gopp::string, isize>* StringToIntP(std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage);
    std::pair<std::map<gopp::string, isize>, gopp::error> GetStringToInt(std::string_view name) const;

    // StringToInt64: a map[string]int64 flag.
    void StringToInt64Var(std::map<gopp::string, int64>* p, std::string_view name, std::map<gopp::string, int64> value, std::string_view usage);
    void StringToInt64VarP(std::map<gopp::string, int64>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage);
    std::map<gopp::string, int64>* StringToInt64(std::string_view name, std::map<gopp::string, int64> value, std::string_view usage);
    std::map<gopp::string, int64>* StringToInt64P(std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage);
    std::pair<std::map<gopp::string, int64>, gopp::error> GetStringToInt64(std::string_view name) const;

private:
    friend struct flagSetAccess;
    static gopp::error notDefined(std::string_view name);
    static gopp::error wrongType(std::string_view want, const gopp::string& have);
    NormalizedName normalizeFlagName(std::string_view name) const;
    gopp::error fail(gopp::error err);
    void usage();
    gopp::error parseArgs(std::span<const gopp::string> args, const std::function<gopp::error(Flag*, gopp::string)>& fn);
    gopp::error parseLongArg(const gopp::string& s, std::span<const gopp::string>& args,
                             const std::function<gopp::error(Flag*, gopp::string)>& fn);
    gopp::error parseShortArg(const gopp::string& s, std::span<const gopp::string>& args,
                              const std::function<gopp::error(Flag*, gopp::string)>& fn);
    gopp::error parseSingleShortArg(gopp::string& shorthands, std::span<const gopp::string>& args,
                                    const std::function<gopp::error(Flag*, gopp::string)>& fn);
    gopp::error handleParseError(gopp::error err);

    gopp::string name_;
    bool parsed_ = false;
    std::unordered_map<NormalizedName, ptr<Flag>> actual_;
    std::vector<Flag*> orderedActual_;
    mutable std::vector<Flag*> sortedActual_;
    std::unordered_map<NormalizedName, ptr<Flag>> formal_;
    std::vector<Flag*> orderedFormal_;
    mutable std::vector<Flag*> sortedFormal_;
    std::map<char, Flag*> shorthands_;
    std::vector<gopp::string> args_;
    isize argsLenAtDash_ = -1;
    ErrorHandling errorHandling_;
    ptr<io::Writer> output_;
    bool interspersed_ = true;
    NormalizeFunc normalizeNameFunc_;
};

// pflag.NewFlagSet
ptr<FlagSet> NewFlagSet(gopp::string name, ErrorHandling errorHandling);

// pflag.CommandLine: the set of the program's flags, named after os::Args()[0],
// which exits on errors.
FlagSet& CommandLine();

// pflag.Usage: prints CommandLine's usage message to standard error;
// CommandLine calls it on errors. Programs may replace it.
extern std::function<void()> Usage;

// The functions on CommandLine.
void VisitAll(const std::function<void(Flag*)>& fn);
void Visit(const std::function<void(Flag*)>& fn);
Flag* Lookup(std::string_view name);
Flag* ShorthandLookup(std::string_view name);
gopp::error Set(std::string_view name, gopp::string value);
void PrintDefaults();
isize NFlag();
gopp::string Arg(isize i);
isize NArg();
const std::vector<gopp::string>& Args();
void Var(ptr<pflag::Value> value, std::string_view name, std::string_view usage);
void VarP(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage);
// pflag.Parse: parses os::Args() after the program's name.
void Parse();
void ParseAll(const std::function<gopp::error(Flag*, gopp::string)>& fn);
void SetInterspersed(bool interspersed);
bool Parsed();

void CountVar(isize* p, std::string_view name, std::string_view usage);
void CountVarP(isize* p, std::string_view name, std::string_view shorthand, std::string_view usage);
isize* Count(std::string_view name, std::string_view usage);
isize* CountP(std::string_view name, std::string_view shorthand, std::string_view usage);
void TimeVar(time::Time* p, std::string_view name, time::Time value, std::vector<gopp::string> formats, std::string_view usage);
void TimeVarP(time::Time* p, std::string_view name, std::string_view shorthand, time::Time value, std::vector<gopp::string> formats,
              std::string_view usage);
time::Time* Time(std::string_view name, time::Time value, std::vector<gopp::string> formats, std::string_view usage);
time::Time* TimeP(std::string_view name, std::string_view shorthand, time::Time value, std::vector<gopp::string> formats,
                  std::string_view usage);
void Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
void FuncP(std::string_view name, std::string_view shorthand, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
void BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
void BoolFuncP(std::string_view name, std::string_view shorthand, std::string_view usage, std::function<gopp::error(gopp::string)> fn);
template <TextValue T>
void TextVar(T* p, std::string_view name, T value, std::string_view usage) {
    CommandLine().TextVarP(p, name, {}, std::move(value), usage);
}
template <TextValue T>
void TextVarP(T* p, std::string_view name, std::string_view shorthand, T value, std::string_view usage) {
    CommandLine().TextVarP(p, name, shorthand, std::move(value), usage);
}

void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
void BoolVarP(bool* p, std::string_view name, std::string_view shorthand, bool value, std::string_view usage);
bool* Bool(std::string_view name, bool value, std::string_view usage);
bool* BoolP(std::string_view name, std::string_view shorthand, bool value, std::string_view usage);
void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
void StringVarP(gopp::string* p, std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage);
gopp::string* String(std::string_view name, gopp::string value, std::string_view usage);
gopp::string* StringP(std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage);
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
void IntVarP(isize* p, std::string_view name, std::string_view shorthand, isize value, std::string_view usage);
isize* Int(std::string_view name, isize value, std::string_view usage);
isize* IntP(std::string_view name, std::string_view shorthand, isize value, std::string_view usage);
void Int8Var(int8* p, std::string_view name, int8 value, std::string_view usage);
void Int8VarP(int8* p, std::string_view name, std::string_view shorthand, int8 value, std::string_view usage);
int8* Int8(std::string_view name, int8 value, std::string_view usage);
int8* Int8P(std::string_view name, std::string_view shorthand, int8 value, std::string_view usage);
void Int16Var(int16* p, std::string_view name, int16 value, std::string_view usage);
void Int16VarP(int16* p, std::string_view name, std::string_view shorthand, int16 value, std::string_view usage);
int16* Int16(std::string_view name, int16 value, std::string_view usage);
int16* Int16P(std::string_view name, std::string_view shorthand, int16 value, std::string_view usage);
void Int32Var(int32* p, std::string_view name, int32 value, std::string_view usage);
void Int32VarP(int32* p, std::string_view name, std::string_view shorthand, int32 value, std::string_view usage);
int32* Int32(std::string_view name, int32 value, std::string_view usage);
int32* Int32P(std::string_view name, std::string_view shorthand, int32 value, std::string_view usage);
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
void Int64VarP(int64* p, std::string_view name, std::string_view shorthand, int64 value, std::string_view usage);
int64* Int64(std::string_view name, int64 value, std::string_view usage);
int64* Int64P(std::string_view name, std::string_view shorthand, int64 value, std::string_view usage);
void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage);
void UintVarP(std::size_t* p, std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage);
std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage);
std::size_t* UintP(std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage);
void Uint8Var(uint8* p, std::string_view name, uint8 value, std::string_view usage);
void Uint8VarP(uint8* p, std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage);
uint8* Uint8(std::string_view name, uint8 value, std::string_view usage);
uint8* Uint8P(std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage);
void Uint16Var(uint16* p, std::string_view name, uint16 value, std::string_view usage);
void Uint16VarP(uint16* p, std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage);
uint16* Uint16(std::string_view name, uint16 value, std::string_view usage);
uint16* Uint16P(std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage);
void Uint32Var(uint32* p, std::string_view name, uint32 value, std::string_view usage);
void Uint32VarP(uint32* p, std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage);
uint32* Uint32(std::string_view name, uint32 value, std::string_view usage);
uint32* Uint32P(std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage);
void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage);
void Uint64VarP(uint64* p, std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage);
uint64* Uint64(std::string_view name, uint64 value, std::string_view usage);
uint64* Uint64P(std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage);
void Float32Var(float* p, std::string_view name, float value, std::string_view usage);
void Float32VarP(float* p, std::string_view name, std::string_view shorthand, float value, std::string_view usage);
float* Float32(std::string_view name, float value, std::string_view usage);
float* Float32P(std::string_view name, std::string_view shorthand, float value, std::string_view usage);
void Float64Var(double* p, std::string_view name, double value, std::string_view usage);
void Float64VarP(double* p, std::string_view name, std::string_view shorthand, double value, std::string_view usage);
double* Float64(std::string_view name, double value, std::string_view usage);
double* Float64P(std::string_view name, std::string_view shorthand, double value, std::string_view usage);
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);
void DurationVarP(time::Duration* p, std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage);
time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage);
time::Duration* DurationP(std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage);
void BytesHexVar(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage);
void BytesHexVarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
slice<byte>* BytesHex(std::string_view name, slice<byte> value, std::string_view usage);
slice<byte>* BytesHexP(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
void BytesBase64Var(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage);
void BytesBase64VarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
slice<byte>* BytesBase64(std::string_view name, slice<byte> value, std::string_view usage);
slice<byte>* BytesBase64P(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage);
void IPVar(net::IP* p, std::string_view name, net::IP value, std::string_view usage);
void IPVarP(net::IP* p, std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage);
net::IP* IP(std::string_view name, net::IP value, std::string_view usage);
net::IP* IPP(std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage);
void IPMaskVar(net::IPMask* p, std::string_view name, net::IPMask value, std::string_view usage);
void IPMaskVarP(net::IPMask* p, std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage);
net::IPMask* IPMask(std::string_view name, net::IPMask value, std::string_view usage);
net::IPMask* IPMaskP(std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage);
void IPNetVar(net::IPNet* p, std::string_view name, net::IPNet value, std::string_view usage);
void IPNetVarP(net::IPNet* p, std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage);
net::IPNet* IPNet(std::string_view name, net::IPNet value, std::string_view usage);
net::IPNet* IPNetP(std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage);
void StringSliceVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage);
void StringSliceVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
std::vector<gopp::string>* StringSlice(std::string_view name, std::vector<gopp::string> value, std::string_view usage);
std::vector<gopp::string>* StringSliceP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
void StringArrayVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage);
void StringArrayVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
std::vector<gopp::string>* StringArray(std::string_view name, std::vector<gopp::string> value, std::string_view usage);
std::vector<gopp::string>* StringArrayP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage);
void IntSliceVar(std::vector<isize>* p, std::string_view name, std::vector<isize> value, std::string_view usage);
void IntSliceVarP(std::vector<isize>* p, std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage);
std::vector<isize>* IntSlice(std::string_view name, std::vector<isize> value, std::string_view usage);
std::vector<isize>* IntSliceP(std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage);
void Int32SliceVar(std::vector<int32>* p, std::string_view name, std::vector<int32> value, std::string_view usage);
void Int32SliceVarP(std::vector<int32>* p, std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage);
std::vector<int32>* Int32Slice(std::string_view name, std::vector<int32> value, std::string_view usage);
std::vector<int32>* Int32SliceP(std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage);
void Int64SliceVar(std::vector<int64>* p, std::string_view name, std::vector<int64> value, std::string_view usage);
void Int64SliceVarP(std::vector<int64>* p, std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage);
std::vector<int64>* Int64Slice(std::string_view name, std::vector<int64> value, std::string_view usage);
std::vector<int64>* Int64SliceP(std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage);
void UintSliceVar(std::vector<std::size_t>* p, std::string_view name, std::vector<std::size_t> value, std::string_view usage);
void UintSliceVarP(std::vector<std::size_t>* p, std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage);
std::vector<std::size_t>* UintSlice(std::string_view name, std::vector<std::size_t> value, std::string_view usage);
std::vector<std::size_t>* UintSliceP(std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage);
void Float32SliceVar(std::vector<float>* p, std::string_view name, std::vector<float> value, std::string_view usage);
void Float32SliceVarP(std::vector<float>* p, std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage);
std::vector<float>* Float32Slice(std::string_view name, std::vector<float> value, std::string_view usage);
std::vector<float>* Float32SliceP(std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage);
void Float64SliceVar(std::vector<double>* p, std::string_view name, std::vector<double> value, std::string_view usage);
void Float64SliceVarP(std::vector<double>* p, std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage);
std::vector<double>* Float64Slice(std::string_view name, std::vector<double> value, std::string_view usage);
std::vector<double>* Float64SliceP(std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage);
void BoolSliceVar(std::vector<bool>* p, std::string_view name, std::vector<bool> value, std::string_view usage);
void BoolSliceVarP(std::vector<bool>* p, std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage);
std::vector<bool>* BoolSlice(std::string_view name, std::vector<bool> value, std::string_view usage);
std::vector<bool>* BoolSliceP(std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage);
void DurationSliceVar(std::vector<time::Duration>* p, std::string_view name, std::vector<time::Duration> value, std::string_view usage);
void DurationSliceVarP(std::vector<time::Duration>* p, std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage);
std::vector<time::Duration>* DurationSlice(std::string_view name, std::vector<time::Duration> value, std::string_view usage);
std::vector<time::Duration>* DurationSliceP(std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage);
void IPSliceVar(std::vector<net::IP>* p, std::string_view name, std::vector<net::IP> value, std::string_view usage);
void IPSliceVarP(std::vector<net::IP>* p, std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage);
std::vector<net::IP>* IPSlice(std::string_view name, std::vector<net::IP> value, std::string_view usage);
std::vector<net::IP>* IPSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage);
void IPNetSliceVar(std::vector<net::IPNet>* p, std::string_view name, std::vector<net::IPNet> value, std::string_view usage);
void IPNetSliceVarP(std::vector<net::IPNet>* p, std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage);
std::vector<net::IPNet>* IPNetSlice(std::string_view name, std::vector<net::IPNet> value, std::string_view usage);
std::vector<net::IPNet>* IPNetSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage);
void StringToStringVar(std::map<gopp::string, gopp::string>* p, std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage);
void StringToStringVarP(std::map<gopp::string, gopp::string>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage);
std::map<gopp::string, gopp::string>* StringToString(std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage);
std::map<gopp::string, gopp::string>* StringToStringP(std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage);
void StringToIntVar(std::map<gopp::string, isize>* p, std::string_view name, std::map<gopp::string, isize> value, std::string_view usage);
void StringToIntVarP(std::map<gopp::string, isize>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage);
std::map<gopp::string, isize>* StringToInt(std::string_view name, std::map<gopp::string, isize> value, std::string_view usage);
std::map<gopp::string, isize>* StringToIntP(std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage);
void StringToInt64Var(std::map<gopp::string, int64>* p, std::string_view name, std::map<gopp::string, int64> value, std::string_view usage);
void StringToInt64VarP(std::map<gopp::string, int64>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage);
std::map<gopp::string, int64>* StringToInt64(std::string_view name, std::map<gopp::string, int64> value, std::string_view usage);
std::map<gopp::string, int64>* StringToInt64P(std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage);

// The errors of parsing.

// pflag.NotExistError: an unknown flag, or one accessed but not defined.
struct NotExistError final : errorInterface {
    enum messageType { flagNotExist, flagNotDefined, flagNoSuchFlag, flagUnknownFlag, flagUnknownShorthandFlag };
    NotExistError(gopp::string name, gopp::string specifiedShorthands, messageType t)
        : name_(std::move(name)), specifiedShorthands_(std::move(specifiedShorthands)), messageType_(t) {}
    gopp::string Error() const override;
    // The name of the flag (without dashes) as it was given.
    gopp::string GetSpecifiedName() const { return name_; }
    // The group of shorthands it was given in, if one.
    gopp::string GetSpecifiedShortnames() const { return specifiedShorthands_; }

private:
    gopp::string name_;
    gopp::string specifiedShorthands_;
    messageType messageType_;
};

// pflag.ValueRequiredError: a flag given without the value it needs.
struct ValueRequiredError final : errorInterface {
    ValueRequiredError(ptr<pflag::Flag> flag, gopp::string specifiedName, gopp::string specifiedShorthands)
        : flag_(std::move(flag)), specifiedName_(std::move(specifiedName)), specifiedShorthands_(std::move(specifiedShorthands)) {}
    gopp::string Error() const override;
    Flag* GetFlag() const { return flag_.get(); }
    gopp::string GetSpecifiedName() const { return specifiedName_; }
    gopp::string GetSpecifiedShortnames() const { return specifiedShorthands_; }

private:
    ptr<pflag::Flag> flag_;
    gopp::string specifiedName_;
    gopp::string specifiedShorthands_;
};

// pflag.InvalidValueError: a value its flag's Set rejected; wraps the
// error of Set.
struct InvalidValueError final : errorInterface {
    InvalidValueError(ptr<pflag::Flag> flag, gopp::string value, gopp::error cause)
        : flag_(std::move(flag)), value_(std::move(value)), cause_(std::move(cause)) {}
    gopp::string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return cause_.get(); }
    Flag* GetFlag() const { return flag_.get(); }
    gopp::string GetValue() const { return value_; }

private:
    ptr<pflag::Flag> flag_;
    gopp::string value_;
    gopp::error cause_;
};

// pflag.InvalidSyntaxError: an argument like "---x" or "--=x".
struct InvalidSyntaxError final : errorInterface {
    explicit InvalidSyntaxError(gopp::string specifiedFlag) : specifiedFlag_(std::move(specifiedFlag)) {}
    gopp::string Error() const override;
    gopp::string GetSpecifiedFlag() const { return specifiedFlag_; }

private:
    gopp::string specifiedFlag_;
};

}  // namespace gopp::pflag
