// After spf13/cobra v1.10.2's args.go.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include "cmd/internal.hpp"

namespace gopp::cmd {

namespace detail {

// Validation of the root command's arguments when it has no Args: unknown
// subcommands are errors, other arguments are accepted.
error legacyArgs(Command& cmd, std::span<const string> args) {
    // No subcommand, always take args.
    if (!cmd.HasSubCommands()) return nullptr;
    // Root command with subcommands, do subcommand checking.
    if (!cmd.HasParent() && !args.empty()) {
        return fmt::Errorf("unknown command {} for {}{}", strconv::Quote(args[0]), strconv::Quote(cmd.CommandPath()),
                           findSuggestions(cmd, args[0].view()));
    }
    return nullptr;
}

}  // namespace detail

error NoArgs(Command& cmd, std::span<const string> args) {
    if (!args.empty()) return fmt::Errorf("unknown command {} for {}", strconv::Quote(args[0]), strconv::Quote(cmd.CommandPath()));
    return nullptr;
}

error OnlyValidArgs(Command& cmd, std::span<const string> args) {
    if (cmd.ValidArgs.empty()) return nullptr;
    // Remove any description that may be included in ValidArgs.
    std::vector<string> validArgs;
    validArgs.reserve(cmd.ValidArgs.size());
    for (auto& v : cmd.ValidArgs) {
        auto tab = v.find('\t');
        validArgs.push_back(tab == string::npos ? v : v.slice(0, tab));
    }
    for (auto& v : args) {
        if (!detail::stringInSlice(v.view(), validArgs)) {
            return fmt::Errorf("invalid argument {} for {}{}", strconv::Quote(v), strconv::Quote(cmd.CommandPath()),
                               detail::findSuggestions(cmd, args[0].view()));
        }
    }
    return nullptr;
}

error ArbitraryArgs(Command&, std::span<const string>) { return nullptr; }

PositionalArgs MinimumNArgs(int n) {
    return [n](Command&, std::span<const string> args) -> error {
        if (static_cast<int>(args.size()) < n) return fmt::Errorf("requires at least {} arg(s), only received {}", n, args.size());
        return nullptr;
    };
}

PositionalArgs MaximumNArgs(int n) {
    return [n](Command&, std::span<const string> args) -> error {
        if (static_cast<int>(args.size()) > n) return fmt::Errorf("accepts at most {} arg(s), received {}", n, args.size());
        return nullptr;
    };
}

PositionalArgs ExactArgs(int n) {
    return [n](Command&, std::span<const string> args) -> error {
        if (static_cast<int>(args.size()) != n) return fmt::Errorf("accepts {} arg(s), received {}", n, args.size());
        return nullptr;
    };
}

PositionalArgs RangeArgs(int min, int max) {
    return [min, max](Command&, std::span<const string> args) -> error {
        int n = static_cast<int>(args.size());
        if (n < min || n > max) return fmt::Errorf("accepts between {} and {} arg(s), received {}", min, max, n);
        return nullptr;
    };
}

PositionalArgs MatchAll(std::vector<PositionalArgs> pargs) {
    return [pargs = std::move(pargs)](Command& cmd, std::span<const string> args) -> error {
        for (auto& parg : pargs) {
            if (auto err = parg(cmd, args)) return err;
        }
        return nullptr;
    };
}

PositionalArgs ExactValidArgs(int n) { return MatchAll(ExactArgs(n), OnlyValidArgs); }

}  // namespace gopp::cmd
