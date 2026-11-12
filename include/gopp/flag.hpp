// The flag package: command-line flags.
//
//     string addr = "127.0.0.1:8080";
//     time::Duration d = 5 * time::Second;
//     flag::StringVar(&addr, "addr", addr, "server address");
//     flag::DurationVar(&d, "d", d, "duration");
//     flag::Parse(argc, argv);
//
// Go's flag.String and flag.Int can't be spelled in C++ (string is the
// type, int a keyword), so only the Var forms are ported. Flags are given
// as -name value, -name=value, or --name; boolean flags as -name or
// -name=false.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <span>
#include <string_view>
#include <vector>
#endif

namespace gopp::flag {

// flag.ErrHelp: -h or -help was given without a help flag defined.
extern const gopp::error ErrHelp;

// flag.FlagSet
class FlagSet {
public:
    explicit FlagSet(gopp::string name) : name_(std::move(name)) {}

    // FlagSet.Init: sets the name shown in the usage message.
    void Init(gopp::string name) { name_ = std::move(name); }

    void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
    void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
    void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
    void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
    void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);

    // Parses the arguments, without the program name. Stops at the first
    // non-flag argument or after "--".
    gopp::error Parse(std::span<const gopp::string> arguments);

    // The arguments after the flags.
    const std::vector<gopp::string>& Args() const noexcept { return args_; }
    bool Parsed() const noexcept { return parsed_; }
    // Prints the flags and their defaults to standard error.
    void PrintDefaults() const;
    const gopp::string& Name() const noexcept { return name_; }

private:
    struct flag_def {
        gopp::string name;
        gopp::string usage;
        gopp::string DefValue;
        const char* type;  // shown in the usage message
        bool IsBool;
        std::function<gopp::error(std::string_view)> set;
    };
    void define(flag_def f);
    gopp::error fail(gopp::string msg);

    gopp::string name_;
    std::vector<flag_def> flags_;
    std::vector<gopp::string> args_;
    bool parsed_ = false;
};

// flag.CommandLine
FlagSet& CommandLine();

void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage);
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage);
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage);
void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage);
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage);

// flag.Parse: parses argv into the command line flags. Prints the usage
// and exits with status 2 on errors, or 0 for -h.
void Parse(int argc, char** argv);

// flag.Args
const std::vector<gopp::string>& Args();

}  // namespace gopp::flag
