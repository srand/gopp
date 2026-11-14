// After spf13/cobra v1.10.2's command.go and cobra.go.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include "cmd/internal.hpp"

#include <algorithm>
#include <string>

namespace gopp::cmd {

using detail::mut;
using detail::S;

bool EnablePrefixMatching = false;
bool EnableCommandSorting = true;
bool EnableCaseInsensitive = false;
bool EnableTraverseRunHooks = false;

namespace {

constexpr std::string_view helpFlagName = "help";
constexpr std::string_view helpCommandName = "help";

std::vector<std::function<void()>>& initializers() {
    static auto* v = new std::vector<std::function<void()>>();
    return *v;
}

std::vector<std::function<void()>>& finalizers() {
    static auto* v = new std::vector<std::function<void()>>();
    return *v;
}

bool commandNameMatches(std::string_view s, std::string_view t) {
    if (EnableCaseInsensitive) return strings::EqualFold(s, t);
    return s == t;
}

bool hasNoOptDefVal(std::string_view name, const pflag::FlagSet* fs) {
    pflag::Flag* flag = fs->Lookup(name);
    return flag && !flag->NoOptDefVal.empty();
}

bool shortHasNoOptDefVal(std::string_view name, const pflag::FlagSet* fs) {
    if (name.empty()) return false;
    pflag::Flag* flag = fs->ShorthandLookup(name.substr(0, 1));
    return flag && !flag->NoOptDefVal.empty();
}

ptr<pflag::FlagSet> newFlagSet(const Command& c) {
    auto& s = S(c);
    auto fs = pflag::NewFlagSet(c.DisplayName(), pflag::ContinueOnError);
    if (!s.flagErrorBuf) s.flagErrorBuf = make_ptr<bytes::Buffer>();
    fs->SetOutput(s.flagErrorBuf);
    return fs;
}

void updateMaxLens(Command& parent, const Command& x) {
    auto& s = S(parent);
    s.commandsMaxUseLen = std::max(s.commandsMaxUseLen, x.Use.size());
    s.commandsMaxCommandPathLen = std::max(s.commandsMaxCommandPathLen, x.CommandPath().size());
    s.commandsMaxNameLen = std::max(s.commandsMaxNameLen, x.Name().size());
}

// Runs the finalizers when it goes out of scope (Go's defer c.postRun()).
struct postRunGuard {
    ~postRunGuard() {
        for (auto& f : finalizers()) f();
    }
};

void checkCommandGroups(const Command& c) {
    for (auto& sub : S(c).commands) {
        // If the command has not been added to a group, it is not an error.
        if (!sub->GroupID.empty() && !c.ContainsGroup(sub->GroupID)) {
            panic(fmt::Sprint("group id '{}' is not defined for subcommand '{}'", sub->GroupID, sub->CommandPath()).view());
        }
        checkCommandGroups(*sub);
    }
}

pflag::Flag* persistentFlag(const Command& c, std::string_view name) {
    pflag::Flag* flag = nullptr;
    if (c.HasPersistentFlags()) flag = c.PersistentFlags()->Lookup(name);
    if (!flag) {
        detail::updateParentsPflags(c);
        flag = S(c).parentsPflags->Lookup(name);
    }
    return flag;
}

// Executes the command once found (Go's execute).
error execute(Command& c, std::span<const string> a) {
    if (!c.Deprecated.empty()) c.Printf("Command {} is deprecated, {}\n", strconv::Quote(c.Name()), c.Deprecated);

    // Initialize help and version flags at the last point possible, to
    // allow for user overriding.
    c.InitDefaultHelpFlag();
    c.InitDefaultVersionFlag();

    if (auto err = c.ParseFlags(a)) return c.FlagErrorFunc()(c, err);

    // If help is called, regardless of other flags, return we want help.
    // Also say we need help if the command isn't runnable.
    auto [helpVal, herr] = c.Flags()->GetBool(helpFlagName);
    if (herr) {
        // Should be impossible to get here as we always declare a help
        // flag in InitDefaultHelpFlag().
        c.Println("\"help\" flag declared as non-bool. Please correct your code");
        return herr;
    }
    if (helpVal) return pflag::ErrHelp;

    // For back-compatibility, version flag behavior only if Version is set.
    if (!c.Version.empty()) {
        auto [versionVal, verr] = c.Flags()->GetBool("version");
        if (verr) {
            c.Println("\"version\" flag declared as non-bool. Please correct your code");
            return verr;
        }
        if (versionVal) {
            const auto* p = &c;
            std::function<error(io::Writer&, const Command&)> fn;
            for (; p; p = p->Parent()) {
                if (S(*p).versionTemplate) {
                    fn = S(*p).versionTemplate->fn;
                    break;
                }
            }
            if (!fn) fn = detail::defaultVersionFunc;
            auto err = fn(c.OutOrStdout(), c);
            if (err) c.Println("{}", err);
            return err;
        }
    }

    if (!c.Runnable()) return pflag::ErrHelp;

    for (auto& f : initializers()) f();
    postRunGuard guard;

    std::vector<string> argWoFlags = c.Flags()->Args();
    if (c.DisableFlagParsing) argWoFlags.assign(a.begin(), a.end());

    if (auto err = c.ValidateArgs(argWoFlags)) return err;

    std::vector<Command*> parents;
    for (Command* p = &c; p; p = p->Parent()) {
        if (EnableTraverseRunHooks) {
            // When EnableTraverseRunHooks is set, execute all persistent
            // pre-runs from the root parent till this command.
            parents.insert(parents.begin(), p);
        } else {
            // Otherwise, execute only the first found persistent hook.
            parents.push_back(p);
        }
    }
    for (Command* p : parents) {
        if (p->PersistentPreRunE) {
            if (auto err = p->PersistentPreRunE(c, argWoFlags)) return err;
            if (!EnableTraverseRunHooks) break;
        } else if (p->PersistentPreRun) {
            p->PersistentPreRun(c, argWoFlags);
            if (!EnableTraverseRunHooks) break;
        }
    }
    if (c.PreRunE) {
        if (auto err = c.PreRunE(c, argWoFlags)) return err;
    } else if (c.PreRun) {
        c.PreRun(c, argWoFlags);
    }

    if (auto err = c.ValidateRequiredFlags()) return err;
    if (auto err = c.ValidateFlagGroups()) return err;

    if (c.RunE) {
        if (auto err = c.RunE(c, argWoFlags)) return err;
    } else {
        c.Run(c, argWoFlags);
    }
    if (c.PostRunE) {
        if (auto err = c.PostRunE(c, argWoFlags)) return err;
    } else if (c.PostRun) {
        c.PostRun(c, argWoFlags);
    }
    for (Command* p = &c; p; p = p->Parent()) {
        if (p->PersistentPostRunE) {
            if (auto err = p->PersistentPostRunE(c, argWoFlags)) return err;
            if (!EnableTraverseRunHooks) break;
        } else if (p->PersistentPostRun) {
            p->PersistentPostRun(c, argWoFlags);
            if (!EnableTraverseRunHooks) break;
        }
    }
    return nullptr;
}

Command* findNext(Command& c, std::string_view next) {
    std::vector<Command*> matches;
    for (auto& cmd : S(c).commands) {
        if (commandNameMatches(cmd->Name().view(), next) || cmd->HasAlias(next)) {
            S(*cmd).calledAsName = string(next);
            return cmd.get();
        }
        if (EnablePrefixMatching) {
            // hasNameOrAliasPrefix
            if (cmd->Name().starts_with(next)) {
                S(*cmd).calledAsName = cmd->Name();
                matches.push_back(cmd.get());
            } else {
                for (auto& alias : cmd->Aliases) {
                    if (alias.starts_with(next)) {
                        S(*cmd).calledAsName = alias;
                        matches.push_back(cmd.get());
                        break;
                    }
                }
            }
        }
    }
    // Only accept a single prefix match - multiple matches would be ambiguous.
    if (matches.size() == 1) return matches[0];
    return nullptr;
}

std::vector<string> argsMinusFirstX(const Command& c, std::span<const string> args, std::string_view x) {
    if (args.empty()) return {args.begin(), args.end()};
    detail::mergePersistentFlags(c);
    const pflag::FlagSet* flags = c.Flags();
    for (std::size_t pos = 0; pos < args.size(); pos++) {
        std::string_view s = args[pos];
        if (s == "--") {
            // -- means we have reached the end of the parseable args.
            break;
        }
        if ((s.starts_with("--") && s.find('=') == s.npos && !hasNoOptDefVal(s.substr(2), flags)) ||
            (s.starts_with("-") && s.find('=') == s.npos && s.size() == 2 && !shortHasNoOptDefVal(s.substr(1), flags))) {
            // A flag that takes the next argument as its value.
            pos++;
            continue;
        }
        if (!s.starts_with("-") && s == x) {
            std::vector<string> ret(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(pos));
            ret.insert(ret.end(), args.begin() + static_cast<std::ptrdiff_t>(pos) + 1, args.end());
            return ret;
        }
    }
    return {args.begin(), args.end()};
}

}  // namespace

// ---------------------------------------------------------------------------
// Package functions

void OnInitialize(std::function<void()> f) { initializers().push_back(std::move(f)); }
void OnFinalize(std::function<void()> f) { finalizers().push_back(std::move(f)); }

void CheckErr(const error& err) {
    if (err) {
        fmt::Println(os::Stderr, "Error: {}", err);
        os::Exit(1);
    }
}

void WriteStringAndCheck(io::Writer& w, std::string_view s) { CheckErr(io::WriteString(w, s).second); }

ptr<Command> New(Command c) { return ptr<Command>(new Command(std::move(c))); }

namespace detail {

string trimRightSpace(std::string_view s) {
    return strings::TrimRightFunc(string(s), [](rune r) { return r == ' ' || r == '\t' || r == '\n' || r == '\r' || r == '\v' || r == '\f' || r == 0x85 || r == 0xA0; });
}

string rpad(std::string_view s, int padding) {
    std::string out(s);
    if (static_cast<int>(out.size()) < padding) out.append(static_cast<std::size_t>(padding) - out.size(), ' ');
    return string(out);
}

std::size_t ld(std::string_view s0, std::string_view t0, bool ignoreCase) {
    std::string s(s0), t(t0);
    if (ignoreCase) {
        s = strings::ToLower(s0).str();
        t = strings::ToLower(t0).str();
    }
    std::vector<std::vector<std::size_t>> d(s.size() + 1, std::vector<std::size_t>(t.size() + 1));
    for (std::size_t i = 0; i <= s.size(); i++) d[i][0] = i;
    for (std::size_t j = 0; j <= t.size(); j++) d[0][j] = j;
    for (std::size_t j = 1; j <= t.size(); j++) {
        for (std::size_t i = 1; i <= s.size(); i++) {
            if (s[i - 1] == t[j - 1]) {
                d[i][j] = d[i - 1][j - 1];
            } else {
                d[i][j] = std::min({d[i - 1][j], d[i][j - 1], d[i - 1][j - 1]}) + 1;
            }
        }
    }
    return d[s.size()][t.size()];
}

bool stringInSlice(std::string_view a, std::span<const string> list) {
    for (auto& b : list) {
        if (b == a) return true;
    }
    return false;
}

bool isFlagArg(std::string_view arg) {
    return (arg.size() >= 3 && arg.substr(0, 2) == "--") || (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-');
}

std::vector<string> stripFlags(std::span<const string> args, const Command& c) {
    if (args.empty()) return {};
    mergePersistentFlags(c);
    std::vector<string> commands;
    const pflag::FlagSet* flags = c.Flags();
    while (!args.empty()) {
        std::string_view s = args[0];
        const string& str = args[0];
        args = args.subspan(1);
        if (s == "--") {
            // "--" terminates the flags.
            break;
        }
        if ((s.starts_with("--") && s.find('=') == s.npos && !hasNoOptDefVal(s.substr(2), flags)) ||
            (s.starts_with("-") && s.find('=') == s.npos && s.size() == 2 && !shortHasNoOptDefVal(s.substr(1), flags))) {
            // If '--flag arg' then delete arg from args. If '-f arg' then
            // delete 'arg' from args.
            if (args.size() <= 1) break;
            args = args.subspan(1);
            continue;
        }
        if (!s.empty() && !s.starts_with("-")) commands.push_back(str);
    }
    return commands;
}

string findSuggestions(Command& c, std::string_view arg) {
    if (c.DisableSuggestions) return {};
    if (c.SuggestionsMinimumDistance <= 0) c.SuggestionsMinimumDistance = 2;
    std::string sb;
    auto suggestions = c.SuggestionsFor(arg);
    if (!suggestions.empty()) {
        sb += "\n\nDid you mean this?\n";
        for (auto& s : suggestions) sb += "\t" + s.str() + "\n";
    }
    return string(sb);
}

void updateParentsPflags(const Command& c) {
    auto& s = S(c);
    if (!s.parentsPflags) {
        s.parentsPflags = newFlagSet(c);
        s.parentsPflags->SortFlags = false;
    }
    if (s.globNormFunc) s.parentsPflags->SetNormalizeFunc(s.globNormFunc);
    c.Root()->PersistentFlags()->AddFlagSet(&pflag::CommandLine());
    mut(c).VisitParents([&](Command& parent) { s.parentsPflags->AddFlagSet(parent.PersistentFlags()); });
}

void mergePersistentFlags(const Command& c) {
    updateParentsPflags(c);
    c.Flags()->AddFlagSet(c.PersistentFlags());
    c.Flags()->AddFlagSet(S(c).parentsPflags.get());
}

commandState::~commandState() { forgetFlagCompletions(*this); }

}  // namespace detail

// ---------------------------------------------------------------------------
// commandHandle

namespace detail {

commandHandle::commandHandle() : s_(std::make_unique<commandState>()) {}
commandHandle::commandHandle(const commandHandle&) : s_(std::make_unique<commandState>()) {}
commandHandle& commandHandle::operator=(const commandHandle&) { return *this; }
commandHandle::~commandHandle() = default;

}  // namespace detail

void Command::releaseRef() const noexcept {
    if (state_.refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
}

// ---------------------------------------------------------------------------
// Context, arguments and streams

ptr<context::Context> Command::Context() { return S(*this).ctx; }
void Command::SetContext(ptr<context::Context> ctx) { S(*this).ctx = std::move(ctx); }
void Command::SetArgs(std::vector<string> a) { S(*this).args = std::move(a); }

void Command::SetOutput(ptr<io::Writer> output) {
    S(*this).outWriter = output;
    S(*this).errWriter = std::move(output);
}
void Command::SetOut(ptr<io::Writer> newOut) { S(*this).outWriter = std::move(newOut); }
void Command::SetErr(ptr<io::Writer> newErr) { S(*this).errWriter = std::move(newErr); }
void Command::SetIn(ptr<io::Reader> newIn) { S(*this).inReader = std::move(newIn); }

io::Writer& Command::OutOrStdout() const {
    for (const Command* c = this; c; c = c->Parent()) {
        if (S(*c).outWriter) return *S(*c).outWriter;
    }
    return os::Stdout;
}

io::Writer& Command::OutOrStderr() const {
    for (const Command* c = this; c; c = c->Parent()) {
        if (S(*c).outWriter) return *S(*c).outWriter;
    }
    return os::Stderr;
}

io::Writer& Command::ErrOrStderr() const {
    for (const Command* c = this; c; c = c->Parent()) {
        if (S(*c).errWriter) return *S(*c).errWriter;
    }
    return os::Stderr;
}

io::Reader& Command::InOrStdin() const {
    for (const Command* c = this; c; c = c->Parent()) {
        if (S(*c).inReader) return *S(*c).inReader;
    }
    return os::Stdin;
}

// ---------------------------------------------------------------------------
// Help, usage and errors

void Command::SetUsageFunc(std::function<error(Command&)> f) { S(*this).usageFunc = std::move(f); }

void Command::SetUsageTemplate(string s) {
    if (s.empty()) {
        S(*this).usageTemplate.reset();
        return;
    }
    S(*this).usageTemplate = detail::tmpl(std::move(s));
}

void Command::SetFlagErrorFunc(std::function<error(Command&, error)> f) { S(*this).flagErrorFunc = std::move(f); }
void Command::SetHelpFunc(std::function<void(Command&, std::span<const string>)> f) { S(*this).helpFunc = std::move(f); }
void Command::SetHelpCommand(ptr<Command> cmd) { S(*this).helpCommand = std::move(cmd); }

void Command::SetHelpCommandGroupID(string groupID) {
    if (S(*this).helpCommand) S(*this).helpCommand->GroupID = groupID;
    // Help command can be initialized after this call.
    S(*this).helpCommandGroupID = std::move(groupID);
}

void Command::SetCompletionCommandGroupID(string groupID) {
    // The completion command is always added to the root.
    S(*Root()).completionCommandGroupID = std::move(groupID);
}

void Command::SetHelpTemplate(string s) {
    if (s.empty()) {
        S(*this).helpTemplate.reset();
        return;
    }
    S(*this).helpTemplate = detail::tmpl(std::move(s));
}

void Command::SetVersionTemplate(string s) {
    if (s.empty()) {
        S(*this).versionTemplate.reset();
        return;
    }
    S(*this).versionTemplate = detail::tmpl(std::move(s));
}

void Command::SetErrPrefix(string s) { S(*this).errPrefix = std::move(s); }

void Command::SetGlobalNormalizationFunc(pflag::FlagSet::NormalizeFunc n) {
    Flags()->SetNormalizeFunc(n);
    PersistentFlags()->SetNormalizeFunc(n);
    S(*this).globNormFunc = n;
    for (auto& command : S(*this).commands) command->SetGlobalNormalizationFunc(n);
}

pflag::FlagSet::NormalizeFunc Command::GlobalNormalizationFunc() const { return S(*this).globNormFunc; }

std::function<error(Command&)> Command::UsageFunc() const {
    if (S(*this).usageFunc) return S(*this).usageFunc;
    if (HasParent()) return Parent()->UsageFunc();
    return [](Command& c) -> error {
        detail::mergePersistentFlags(c);
        std::function<error(io::Writer&, const Command&)> fn = detail::defaultUsageFunc;
        for (const Command* p = &c; p; p = p->Parent()) {
            if (S(*p).usageTemplate) {
                fn = S(*p).usageTemplate->fn;
                break;
            }
        }
        auto err = fn(c.OutOrStderr(), c);
        if (err) c.PrintErrln("{}", err);
        return err;
    };
}

error Command::Usage() const { return UsageFunc()(mut(*this)); }

std::function<void(Command&, std::span<const string>)> Command::HelpFunc() const {
    if (S(*this).helpFunc) return S(*this).helpFunc;
    if (HasParent()) return Parent()->HelpFunc();
    return [](Command& c, std::span<const string>) {
        detail::mergePersistentFlags(c);
        std::function<error(io::Writer&, const Command&)> fn = detail::defaultHelpFunc;
        for (const Command* p = &c; p; p = p->Parent()) {
            if (S(*p).helpTemplate) {
                fn = S(*p).helpTemplate->fn;
                break;
            }
        }
        // The help should be sent to stdout. See
        // https://github.com/spf13/cobra/issues/1002
        if (auto err = fn(c.OutOrStdout(), c)) c.PrintErrln("{}", err);
    };
}

error Command::Help() const {
    HelpFunc()(mut(*this), {});
    return nullptr;
}

string Command::UsageString() const {
    // Storing normal writers.
    auto& s = S(*this);
    auto tmpOutput = s.outWriter;
    auto tmpErr = s.errWriter;
    auto bb = make_ptr<bytes::Buffer>();
    s.outWriter = bb;
    s.errWriter = bb;
    CheckErr(Usage());
    // Setting things back to normal.
    s.outWriter = tmpOutput;
    s.errWriter = tmpErr;
    return bb->String();
}

std::function<error(Command&, error)> Command::FlagErrorFunc() const {
    if (S(*this).flagErrorFunc) return S(*this).flagErrorFunc;
    if (HasParent()) return Parent()->FlagErrorFunc();
    return [](Command&, error err) { return err; };
}

int Command::UsagePadding() const {
    constexpr std::size_t minUsagePadding = 25;
    if (!HasParent() || minUsagePadding > S(*Parent()).commandsMaxUseLen) return minUsagePadding;
    return static_cast<int>(S(*Parent()).commandsMaxUseLen);
}

int Command::CommandPathPadding() const {
    constexpr std::size_t minCommandPathPadding = 11;
    if (!HasParent() || minCommandPathPadding > S(*Parent()).commandsMaxCommandPathLen) return minCommandPathPadding;
    return static_cast<int>(S(*Parent()).commandsMaxCommandPathLen);
}

int Command::NamePadding() const {
    constexpr std::size_t minNamePadding = 11;
    if (!HasParent() || minNamePadding > S(*Parent()).commandsMaxNameLen) return minNamePadding;
    return static_cast<int>(S(*Parent()).commandsMaxNameLen);
}

namespace {

constexpr std::string_view defaultUsageTemplate = R"(Usage:{{if .Runnable}}
  {{.UseLine}}{{end}}{{if .HasAvailableSubCommands}}
  {{.CommandPath}} [command]{{end}}{{if gt (len .Aliases) 0}}

Aliases:
  {{.NameAndAliases}}{{end}}{{if .HasExample}}

Examples:
{{.Example}}{{end}}{{if .HasAvailableSubCommands}}{{$cmds := .Commands}}{{if eq (len .Groups) 0}}

Available Commands:{{range $cmds}}{{if (or .IsAvailableCommand (eq .Name "help"))}}
  {{rpad .Name .NamePadding }} {{.Short}}{{end}}{{end}}{{else}}{{range $group := .Groups}}

{{.Title}}{{range $cmds}}{{if (and (eq .GroupID $group.ID) (or .IsAvailableCommand (eq .Name "help")))}}
  {{rpad .Name .NamePadding }} {{.Short}}{{end}}{{end}}{{end}}{{if not .AllChildCommandsHaveGroup}}

Additional Commands:{{range $cmds}}{{if (and (eq .GroupID "") (or .IsAvailableCommand (eq .Name "help")))}}
  {{rpad .Name .NamePadding }} {{.Short}}{{end}}{{end}}{{end}}{{end}}{{end}}{{if .HasAvailableLocalFlags}}

Flags:
{{.LocalFlags.FlagUsages | trimTrailingWhitespaces}}{{end}}{{if .HasAvailableInheritedFlags}}

Global Flags:
{{.InheritedFlags.FlagUsages | trimTrailingWhitespaces}}{{end}}{{if .HasHelpSubCommands}}

Additional help topics:{{range .Commands}}{{if .IsAdditionalHelpTopicCommand}}
  {{rpad .CommandPath .CommandPathPadding}} {{.Short}}{{end}}{{end}}{{end}}{{if .HasAvailableSubCommands}}

Use "{{.CommandPath}} [command] --help" for more information about a command.{{end}}
)";

constexpr std::string_view defaultHelpTemplate = R"({{with (or .Long .Short)}}{{. | trimTrailingWhitespaces}}

{{end}}{{if or .Runnable .HasSubCommands}}{{.UsageString}}{{end}})";

constexpr std::string_view defaultVersionTemplate = R"({{with .DisplayName}}{{printf "%s " .}}{{end}}{{printf "version %s" .Version}}
)";

}  // namespace

string Command::UsageTemplate() const {
    if (S(*this).usageTemplate) return S(*this).usageTemplate->tmpl;
    if (HasParent()) return Parent()->UsageTemplate();
    return string::fromStatic(defaultUsageTemplate);
}

string Command::HelpTemplate() const {
    if (S(*this).helpTemplate) return S(*this).helpTemplate->tmpl;
    if (HasParent()) return Parent()->HelpTemplate();
    return string::fromStatic(defaultHelpTemplate);
}

string Command::VersionTemplate() const {
    if (S(*this).versionTemplate) return S(*this).versionTemplate->tmpl;
    if (HasParent()) return Parent()->VersionTemplate();
    return string::fromStatic(defaultVersionTemplate);
}

string Command::ErrPrefix() const {
    if (!S(*this).errPrefix.empty()) return S(*this).errPrefix;
    if (HasParent()) return Parent()->ErrPrefix();
    return "Error:"_s;
}

// ---------------------------------------------------------------------------
// Finding and running commands

std::tuple<Command*, std::vector<string>, error> Command::Find(std::span<const string> args) {
    std::function<std::pair<Command*, std::vector<string>>(Command*, std::vector<string>)> innerfind =
        [&](Command* c, std::vector<string> innerArgs) -> std::pair<Command*, std::vector<string>> {
        auto argsWOflags = detail::stripFlags(innerArgs, *c);
        if (argsWOflags.empty()) return {c, std::move(innerArgs)};
        const string& nextSubCmd = argsWOflags[0];
        if (Command* cmd = findNext(*c, nextSubCmd.view())) {
            return innerfind(cmd, argsMinusFirstX(*c, innerArgs, nextSubCmd.view()));
        }
        return {c, std::move(innerArgs)};
    };
    auto [commandFound, a] = innerfind(this, std::vector<string>(args.begin(), args.end()));
    if (!commandFound->Args) {
        auto stripped = detail::stripFlags(a, *commandFound);
        auto err = detail::legacyArgs(*commandFound, stripped);
        return {commandFound, std::move(a), err};
    }
    return {commandFound, std::move(a), nullptr};
}

std::tuple<Command*, std::vector<string>, error> Command::Traverse(std::span<const string> args) {
    std::vector<string> flags;
    bool inFlag = false;
    for (std::size_t i = 0; i < args.size(); i++) {
        std::string_view arg = args[i];
        if (arg.starts_with("--") && arg.find('=') == arg.npos) {
            // A long flag with a space separated value: --foo bar.
            inFlag = !hasNoOptDefVal(arg.substr(2), Flags());
            flags.push_back(args[i]);
            continue;
        }
        if (arg.starts_with("-") && arg.find('=') == arg.npos && arg.size() == 2 && !shortHasNoOptDefVal(arg.substr(1), Flags())) {
            // A short flag with a space separated value: -f bar.
            inFlag = true;
            flags.push_back(args[i]);
            continue;
        }
        if (inFlag) {
            // The value for a flag.
            inFlag = false;
            flags.push_back(args[i]);
            continue;
        }
        if (detail::isFlagArg(arg)) {
            // A flag without a value, or with an = separated value.
            flags.push_back(args[i]);
            continue;
        }
        Command* cmd = findNext(*this, arg);
        if (!cmd) return {this, std::vector<string>(args.begin(), args.end()), nullptr};
        if (auto err = ParseFlags(flags)) return {nullptr, std::vector<string>(args.begin(), args.end()), err};
        return cmd->Traverse(args.subspan(i + 1));
    }
    return {this, std::vector<string>(args.begin(), args.end()), nullptr};
}

std::vector<string> Command::SuggestionsFor(std::string_view typedName) const {
    std::vector<string> suggestions;
    for (auto& cmd : S(*this).commands) {
        if (!cmd->IsAvailableCommand()) continue;
        std::size_t levenshteinDistance = detail::ld(typedName, cmd->Name().view(), true);
        bool suggestByLevenshtein = static_cast<int>(levenshteinDistance) <= SuggestionsMinimumDistance;
        bool suggestByPrefix = strings::HasPrefix(strings::ToLower(cmd->Name()), strings::ToLower(typedName));
        if (suggestByLevenshtein || suggestByPrefix) suggestions.push_back(cmd->Name());
        for (auto& explicitSuggestion : cmd->SuggestFor) {
            if (strings::EqualFold(typedName, explicitSuggestion)) suggestions.push_back(cmd->Name());
        }
    }
    return suggestions;
}

void Command::VisitParents(const std::function<void(Command&)>& fn) {
    if (HasParent()) {
        fn(*Parent());
        Parent()->VisitParents(fn);
    }
}

Command* Command::Root() const {
    const Command* c = this;
    while (c->HasParent()) c = c->Parent();
    return &mut(*c);
}

Command* Command::Parent() const { return S(*this).parent; }
bool Command::HasParent() const { return S(*this).parent != nullptr; }
isize Command::ArgsLenAtDash() const { return Flags()->ArgsLenAtDash(); }

error Command::Execute() { return ExecuteC().second; }

error Command::ExecuteContext(ptr<context::Context> ctx) {
    S(*this).ctx = std::move(ctx);
    return Execute();
}

std::pair<Command*, error> Command::ExecuteContextC(ptr<context::Context> ctx) {
    S(*this).ctx = std::move(ctx);
    return ExecuteC();
}

std::pair<Command*, error> Command::ExecuteC() {
    if (!S(*this).ctx) S(*this).ctx = context::Background();

    // Regardless of what command execute is called on, run on Root only.
    if (HasParent()) return Root()->ExecuteC();

    // Initialize help at the last point to allow for user overriding.
    InitDefaultHelpCmd();

    std::vector<string> args;
    if (S(*this).args) {
        args = *S(*this).args;
    } else if (!os::Args().empty()) {
        args.assign(os::Args().begin() + 1, os::Args().end());
    }

    // Initialize the __complete command to be used for shell completion.
    detail::initCompleteCmd(*this, args);
    // Initialize the default completion command.
    InitDefaultCompletionCmd(args);
    // Now that all commands have been created, let's make sure all groups
    // are properly created for all commands.
    checkCommandGroups(*this);

    Command* cmd = nullptr;
    std::vector<string> flags;
    error err;
    if (TraverseChildren) std::tie(cmd, flags, err) = Traverse(args);
    else std::tie(cmd, flags, err) = Find(args);
    Command* c = this;
    if (err) {
        // If found parse to a subcommand and then failed, talk about the
        // subcommand.
        if (cmd) c = cmd;
        if (!c->SilenceErrors) {
            c->PrintErrln("{} {}", c->ErrPrefix(), err.Error());
            c->PrintErrf("Run '{} --help' for usage.\n", c->CommandPath());
        }
        return {c, err};
    }

    S(*cmd).calledAsCalled = true;
    if (S(*cmd).calledAsName.empty()) S(*cmd).calledAsName = cmd->Name();

    // We have to pass global context to children command if context is
    // present on the parent command.
    if (!S(*cmd).ctx) S(*cmd).ctx = S(*c).ctx;

    err = execute(*cmd, flags);
    if (err) {
        // Always show help if requested, even if SilenceErrors is in
        // effect.
        if (errors::Is(err, pflag::ErrHelp)) {
            cmd->HelpFunc()(*cmd, args);
            return {cmd, nullptr};
        }
        // If root command has SilenceErrors flagged, all subcommands are
        // inherited.
        if (!cmd->SilenceErrors && !c->SilenceErrors) c->PrintErrln("{} {}", cmd->ErrPrefix(), err.Error());
        // If root command has SilenceUsage flagged, all subcommands are
        // inherited.
        if (!cmd->SilenceUsage && !c->SilenceUsage) c->Println("{}", cmd->UsageString());
    }
    return {cmd, err};
}

error Command::ValidateArgs(std::span<const string> args) {
    if (!Args) return ArbitraryArgs(*this, args);
    return Args(*this, args);
}

error Command::ValidateRequiredFlags() {
    if (DisableFlagParsing) return nullptr;
    std::vector<string> missingFlagNames;
    Flags()->VisitAll([&](pflag::Flag* f) {
        auto it = f->Annotations.find(string::fromStatic(BashCompOneRequiredFlag));
        if (it == f->Annotations.end()) return;
        if (!it->second.empty() && it->second[0] == "true" && !f->Changed) missingFlagNames.push_back(f->Name);
    });
    if (!missingFlagNames.empty()) {
        return fmt::Errorf("required flag(s) \"{}\" not set", strings::Join(missingFlagNames, "\", \""));
    }
    return nullptr;
}

void Command::InitDefaultHelpFlag() {
    detail::mergePersistentFlags(*this);
    if (!Flags()->Lookup(helpFlagName)) {
        std::string usage = "help for ";
        string name = DisplayName();
        if (name.empty()) usage += "this command";
        else usage += name.view();
        Flags()->BoolP(helpFlagName, "h", false, usage);
        Flags()->SetAnnotation(helpFlagName, FlagSetByCobraAnnotation, {"true"_s});
    }
}

void Command::InitDefaultVersionFlag() {
    if (Version.empty()) return;
    detail::mergePersistentFlags(*this);
    if (!Flags()->Lookup("version")) {
        std::string usage = "version for ";
        if (Name().empty()) usage += "this command";
        else usage += DisplayName().view();
        if (!Flags()->ShorthandLookup("v")) Flags()->BoolP("version", "v", false, usage);
        else Flags()->Bool("version", false, usage);
        Flags()->SetAnnotation("version", FlagSetByCobraAnnotation, {"true"_s});
    }
}

void Command::InitDefaultHelpCmd() {
    if (!HasSubCommands()) return;
    auto& s = S(*this);
    if (!s.helpCommand) {
        s.helpCommand = New({
            .Use = "help [command]",
            .Short = "Help about any command",
            .Long = "Help provides help for any command in the application.\nSimply type "_s + DisplayName() +
                    " help [path to command] for full details.",
            .ValidArgsFunction = [](Command& c, std::span<const string> args,
                                    std::string_view toComplete) -> std::pair<std::vector<Completion>, ShellCompDirective> {
                std::vector<Completion> completions;
                auto [cmd, _, e] = c.Root()->Find(args);
                if (e) return {{}, ShellCompDirectiveNoFileComp};
                if (!cmd) {
                    // Root help command.
                    cmd = c.Root();
                }
                for (auto& subCmd : cmd->Commands()) {
                    if (subCmd->IsAvailableCommand() || subCmd == S(*cmd).helpCommand) {
                        if (subCmd->Name().starts_with(toComplete)) {
                            completions.push_back(CompletionWithDesc(subCmd->Name(), subCmd->Short));
                        }
                    }
                }
                return {completions, ShellCompDirectiveNoFileComp};
            },
            .Run =
                [](Command& c, std::span<const string> args) {
                    auto [cmd, _, e] = c.Root()->Find(args);
                    if (!cmd || e) {
                        // Go's %#q of a []string: [`a` `b`].
                        std::string topics = "[";
                        for (std::size_t i = 0; i < args.size(); i++) {
                            if (i > 0) topics += ' ';
                            topics += "`" + args[i].str() + "`";
                        }
                        c.Printf("Unknown help topic {}\n", topics + "]");
                        CheckErr(c.Root()->Usage());
                    } else {
                        // FLow the context down to be used in help text.
                        if (!S(*cmd).ctx) S(*cmd).ctx = S(c).ctx;
                        cmd->InitDefaultHelpFlag();     // make possible 'help' flag to be shown
                        cmd->InitDefaultVersionFlag();  // make possible 'version' flag to be shown
                        CheckErr(cmd->Help());
                    }
                },
        });
        s.helpCommand->GroupID = s.helpCommandGroupID;
    }
    RemoveCommand(s.helpCommand);
    AddCommand(s.helpCommand);
}

void Command::ResetCommands() {
    auto& s = S(*this);
    s.parent = nullptr;
    s.commands.clear();
    s.helpCommand = nullptr;
    s.parentsPflags = nullptr;
}

// ---------------------------------------------------------------------------
// The tree

const std::vector<ptr<Command>>& Command::Commands() const {
    auto& s = S(*this);
    // Do not sort commands if it already sorted or sorting was disabled.
    if (EnableCommandSorting && !s.commandsAreSorted) {
        std::stable_sort(s.commands.begin(), s.commands.end(), [](auto& a, auto& b) { return a->Name() < b->Name(); });
        s.commandsAreSorted = true;
    }
    return s.commands;
}

void Command::AddCommands(std::span<const ptr<Command>> cmds) {
    auto& s = S(*this);
    for (auto& x : cmds) {
        if (x.get() == this) panic("Command can't be a child of itself");
        S(*x).parent = this;
        updateMaxLens(*this, *x);
        // If global normalization function exists, update all children.
        if (s.globNormFunc) x->SetGlobalNormalizationFunc(s.globNormFunc);
        s.commands.push_back(x);
        s.commandsAreSorted = false;
    }
}

void Command::RemoveCommands(std::span<const ptr<Command>> cmds) {
    auto& s = S(*this);
    std::vector<ptr<Command>> commands;
    for (auto& command : s.commands) {
        bool removed = false;
        for (auto& c : cmds) {
            if (command == c) {
                S(*command).parent = nullptr;
                removed = true;
                break;
            }
        }
        if (!removed) commands.push_back(command);
    }
    s.commands = std::move(commands);
    // Recompute all lengths.
    s.commandsMaxUseLen = 0;
    s.commandsMaxCommandPathLen = 0;
    s.commandsMaxNameLen = 0;
    for (auto& command : s.commands) updateMaxLens(*this, *command);
}

const std::vector<Group>& Command::Groups() const { return S(*this).commandgroups; }

void Command::AddGroups(std::span<const Group> groups) {
    auto& g = S(*this).commandgroups;
    g.insert(g.end(), groups.begin(), groups.end());
}

bool Command::AllChildCommandsHaveGroup() const {
    for (auto& sub : S(*this).commands) {
        if ((sub->IsAvailableCommand() || sub == S(*this).helpCommand) && sub->GroupID.empty()) return false;
    }
    return true;
}

bool Command::ContainsGroup(std::string_view groupID) const {
    for (auto& x : S(*this).commandgroups) {
        if (x.ID == groupID) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Output

void Command::Print(std::string_view s) const { io::WriteString(OutOrStderr(), s); }
void Command::PrintErr(std::string_view s) const { io::WriteString(ErrOrStderr(), s); }

// ---------------------------------------------------------------------------
// Names

string Command::CommandPath() const {
    if (HasParent()) return Parent()->CommandPath() + " " + Name();
    return DisplayName();
}

string Command::DisplayName() const {
    if (auto it = Annotations.find(string::fromStatic(CommandDisplayNameAnnotation)); it != Annotations.end()) return it->second;
    return Name();
}

string Command::UseLine() const {
    string use = Use;
    // Replace the name with the display name, once.
    string name = Name();
    if (auto i = use.find(name.view()); i != string::npos) use = use.slice(0, i) + DisplayName() + use.slice(i + name.size());
    string useline = HasParent() ? Parent()->CommandPath() + " " + use : use;
    if (DisableFlagsInUseLine) return useline;
    if (HasAvailableFlags() && !strings::Contains(useline, "[flags]")) useline = useline + " [flags]";
    return useline;
}

void Command::DebugFlags() {
    Println("DebugFlags called on {}", Name());
    std::function<void(Command&)> debugflags = [&](Command& x) {
        if (x.HasFlags() || x.HasPersistentFlags()) Println("{}", x.Name());
        auto line = [&](pflag::Flag* f, std::string_view kind) {
            Println("  -{}, --{} [{}]  {}   [{}]", f->Shorthand, f->Name, f->DefValue, f->Value->String(), kind);
        };
        if (x.HasFlags()) {
            x.Flags()->VisitAll([&](pflag::Flag* f) {
                if (x.HasPersistentFlags() && persistentFlag(x, f->Name.view())) line(f, "LP");
                else line(f, "L");
            });
        }
        if (x.HasPersistentFlags()) {
            x.PersistentFlags()->VisitAll([&](pflag::Flag* f) {
                if (x.HasFlags()) {
                    if (!x.Flags()->Lookup(f->Name.view())) line(f, "P");
                } else {
                    line(f, "P");
                }
            });
        }
        Println("{}", S(x).flagErrorBuf ? S(x).flagErrorBuf->String() : string());
        if (x.HasSubCommands()) {
            for (auto& y : S(x).commands) debugflags(*y);
        }
    };
    debugflags(*this);
}

string Command::Name() const {
    auto i = Use.find(' ');
    if (i != string::npos) return Use.slice(0, i);
    return Use;
}

bool Command::HasAlias(std::string_view s) const {
    for (auto& a : Aliases) {
        if (commandNameMatches(a.view(), s)) return true;
    }
    return false;
}

string Command::CalledAs() const {
    if (S(*this).calledAsCalled) return S(*this).calledAsName;
    return {};
}

string Command::NameAndAliases() const {
    std::vector<string> names{Name()};
    names.insert(names.end(), Aliases.begin(), Aliases.end());
    return strings::Join(names, ", ");
}

bool Command::HasExample() const { return !Example.empty(); }
bool Command::Runnable() const { return static_cast<bool>(Run) || static_cast<bool>(RunE); }
bool Command::HasSubCommands() const { return !S(*this).commands.empty(); }

bool Command::IsAvailableCommand() const {
    if (!Deprecated.empty() || Hidden) return false;
    if (HasParent() && S(*Parent()).helpCommand.get() == this) return false;
    return Runnable() || HasAvailableSubCommands();
}

bool Command::IsAdditionalHelpTopicCommand() const {
    // If a command is runnable, deprecated, or hidden it is not a 'help'
    // command.
    if (Runnable() || !Deprecated.empty() || Hidden) return false;
    // If any of the child commands are not 'help' commands, the command is
    // not a 'help' command.
    for (auto& sub : S(*this).commands) {
        if (!sub->IsAdditionalHelpTopicCommand()) return false;
    }
    return true;
}

bool Command::HasHelpSubCommands() const {
    for (auto& sub : S(*this).commands) {
        if (sub->IsAdditionalHelpTopicCommand()) return true;
    }
    return false;
}

bool Command::HasAvailableSubCommands() const {
    for (auto& sub : S(*this).commands) {
        if (sub->IsAvailableCommand()) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Flags

pflag::FlagSet* Command::Flags() const {
    auto& s = S(*this);
    if (!s.flags) s.flags = newFlagSet(*this);
    return s.flags.get();
}

ptr<pflag::FlagSet> Command::LocalNonPersistentFlags() const {
    pflag::FlagSet* persistentFlags = PersistentFlags();
    auto out = pflag::NewFlagSet(DisplayName(), pflag::ContinueOnError);
    LocalFlags()->VisitAll([&](pflag::Flag* f) {
        if (!persistentFlags->Lookup(f->Name.view())) out->AddFlag(f);
    });
    return out;
}

pflag::FlagSet* Command::LocalFlags() const {
    detail::mergePersistentFlags(*this);
    auto& s = S(*this);
    if (!s.lflags) s.lflags = newFlagSet(*this);
    s.lflags->SortFlags = Flags()->SortFlags;
    if (s.globNormFunc) s.lflags->SetNormalizeFunc(s.globNormFunc);
    auto addToLocal = [&](pflag::Flag* f) {
        // Add the flag if it is not a parent PFlag, or it shadows a parent
        // PFlag.
        if (!s.lflags->Lookup(f->Name.view()) && f != s.parentsPflags->Lookup(f->Name.view())) s.lflags->AddFlag(f);
    };
    Flags()->VisitAll(addToLocal);
    PersistentFlags()->VisitAll(addToLocal);
    return s.lflags.get();
}

pflag::FlagSet* Command::InheritedFlags() const {
    detail::mergePersistentFlags(*this);
    auto& s = S(*this);
    if (!s.iflags) s.iflags = newFlagSet(*this);
    pflag::FlagSet* local = LocalFlags();
    if (s.globNormFunc) s.iflags->SetNormalizeFunc(s.globNormFunc);
    s.parentsPflags->VisitAll([&](pflag::Flag* f) {
        if (!s.iflags->Lookup(f->Name.view()) && !local->Lookup(f->Name.view())) s.iflags->AddFlag(f);
    });
    return s.iflags.get();
}

pflag::FlagSet* Command::NonInheritedFlags() const { return LocalFlags(); }

pflag::FlagSet* Command::PersistentFlags() const {
    auto& s = S(*this);
    if (!s.pflags) s.pflags = newFlagSet(*this);
    return s.pflags.get();
}

void Command::ResetFlags() {
    auto& s = S(*this);
    s.flagErrorBuf = make_ptr<bytes::Buffer>();
    s.flags = newFlagSet(*this);
    s.pflags = newFlagSet(*this);
    s.lflags = nullptr;
    s.iflags = nullptr;
    s.parentsPflags = nullptr;
}

bool Command::HasFlags() const { return Flags()->HasFlags(); }
bool Command::HasPersistentFlags() const { return PersistentFlags()->HasFlags(); }
bool Command::HasLocalFlags() const { return LocalFlags()->HasFlags(); }
bool Command::HasInheritedFlags() const { return InheritedFlags()->HasFlags(); }
bool Command::HasAvailableFlags() const { return Flags()->HasAvailableFlags(); }
bool Command::HasAvailablePersistentFlags() const { return PersistentFlags()->HasAvailableFlags(); }
bool Command::HasAvailableLocalFlags() const { return LocalFlags()->HasAvailableFlags(); }
bool Command::HasAvailableInheritedFlags() const { return InheritedFlags()->HasAvailableFlags(); }

pflag::Flag* Command::Flag(std::string_view name) const {
    pflag::Flag* flag = Flags()->Lookup(name);
    if (!flag) flag = persistentFlag(*this, name);
    return flag;
}

error Command::ParseFlags(std::span<const string> args) {
    if (DisableFlagParsing) return nullptr;
    auto& s = S(*this);
    if (!s.flagErrorBuf) s.flagErrorBuf = make_ptr<bytes::Buffer>();
    isize beforeErrorBufLen = s.flagErrorBuf->Len();
    detail::mergePersistentFlags(*this);
    // Do it here after merging all flags and just before parse.
    Flags()->ParseErrorsAllowlist = FParseErrWhitelist;
    auto err = Flags()->Parse(args);
    // Print warnings if they occurred (e.g. deprecated flag messages).
    if (s.flagErrorBuf->Len() > beforeErrorBufLen && !err) Print(s.flagErrorBuf->String());
    return err;
}

}  // namespace gopp::cmd
