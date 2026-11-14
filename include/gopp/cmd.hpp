// The cmd package: command-line programs with subcommands, ported from
// spf13/cobra v1.10.2, the library of kubectl, gh and hugo, over
// gopp::pflag's GNU-style flags.
//
//     auto root = cmd::New({
//         .Use = "todo",
//         .Short = "A tiny todo list",
//     });
//     bool all = false;
//     auto list = cmd::New({
//         .Use = "list",
//         .Aliases = {"ls"},
//         .Short = "List the items",
//         .Args = cmd::NoArgs,
//         .RunE = [&](cmd::Command& c, std::span<const string> args) -> error {
//             fmt::Println(c.OutOrStdout(), "all: {}", all);
//             return nullptr;
//         },
//     });
//     list->Flags()->BoolVarP(&all, "all", "a", false, "show done items too");
//     root->AddCommand(list);
//     return root->Execute() ? 1 : 0;
//
// A Command is an aggregate, so it takes designated initializers, in the
// order of its fields (cobra's order); cmd::New allocates one, as
// &cobra.Command{...} does. Commands live in ptr<Command>: a parent holds
// its children. Callbacks get the command as Command& and the arguments as
// a span; Go's *Command results are Command* here.
//
// What cobra does, this does: help and usage output, a help command and
// --help/-h, --version, argument validators, persistent flags and pre/post
// run hooks, required and grouped flags, command groups, aliases and
// "did you mean" suggestions, help and usage templates (text/template over
// the Command, with cobra's functions), and shell completion for bash, zsh,
// fish and PowerShell through the hidden __complete command, with dynamic
// completions (ValidArgsFunction, RegisterFlagCompletionFunc) and active
// help. Not ported: the legacy bash completion (V1; GenBashCompletionV2
// is the one kubectl uses), the cobra/doc generators, and Windows'
// mousetrap.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/pflag.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/text/template.hpp>

#include <atomic>
#include <concepts>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::cmd {

struct Command;

// Annotations cobra sets on flags and commands.
// A flag cobra defined (--help, --version).
inline constexpr std::string_view FlagSetByCobraAnnotation = "cobra_annotation_flag_set_by_cobra";
// The name to show for a command instead of its Name(), for plugins run
// as `kubectl foo`: Annotations[CommandDisplayNameAnnotation] = "kubectl foo".
inline constexpr std::string_view CommandDisplayNameAnnotation = "cobra_annotation_command_display_name";

// Flag annotations for shell completion.
inline constexpr std::string_view BashCompFilenameExt = "cobra_annotation_bash_completion_filename_extensions";
inline constexpr std::string_view BashCompCustom = "cobra_annotation_bash_completion_custom";
inline constexpr std::string_view BashCompOneRequiredFlag = "cobra_annotation_bash_completion_one_required_flag";
inline constexpr std::string_view BashCompSubdirsInDir = "cobra_annotation_bash_completion_subdirs_in_dir";

// The hidden commands that shells call to complete a command line.
inline constexpr std::string_view ShellCompRequestCmd = "__complete";
inline constexpr std::string_view ShellCompNoDescRequestCmd = "__completeNoDesc";

// cobra.FParseErrWhitelist: flag parsing errors to ignore.
using FParseErrWhitelist = pflag::ParseErrorsAllowlist;

// cobra.Group: a group of subcommands in the help output.
struct Group {
    string ID{};
    string Title{};
};

// cobra.PositionalArgs: validates a command's arguments (after flags).
using PositionalArgs = std::function<error(Command& cmd, std::span<const string> args)>;

// The hooks and Run functions.
using RunFunc = std::function<void(Command& cmd, std::span<const string> args)>;
using RunEFunc = std::function<error(Command& cmd, std::span<const string> args)>;

// cobra.ShellCompDirective: how the shell should treat completions. Bits.
enum ShellCompDirective : int {
    // An error occurred; completions should be ignored.
    ShellCompDirectiveError = 1 << 0,
    // No space after the completion, even if there's a single one.
    ShellCompDirectiveNoSpace = 1 << 1,
    // No file completion, even if there are no completions.
    ShellCompDirectiveNoFileComp = 1 << 2,
    // The completions are file extensions to filter files by.
    ShellCompDirectiveFilterFileExt = 1 << 3,
    // Complete directory names only; a completion, if any, is the
    // directory to look in.
    ShellCompDirectiveFilterDirs = 1 << 4,
    // Keep the completions' order instead of sorting them.
    ShellCompDirectiveKeepOrder = 1 << 5,
    // Complete files if there are no completions.
    ShellCompDirectiveDefault = 0,
};
constexpr ShellCompDirective operator|(ShellCompDirective a, ShellCompDirective b) noexcept {
    return static_cast<ShellCompDirective>(static_cast<int>(a) | static_cast<int>(b));
}
constexpr ShellCompDirective operator&(ShellCompDirective a, ShellCompDirective b) noexcept {
    return static_cast<ShellCompDirective>(static_cast<int>(a) & static_cast<int>(b));
}

// cobra.Completion: a completion, with an optional description after a
// tab (CompletionWithDesc).
using Completion = string;

// cobra.CompletionFunc: the completions of an argument or a flag's value,
// given the arguments before it and the text typed so far.
using CompletionFunc =
    std::function<std::pair<std::vector<Completion>, ShellCompDirective>(Command& cmd, std::span<const string> args, std::string_view toComplete)>;

// cobra.CompletionOptions: the default `completion` command and the
// completion behavior.
struct CompletionOptions {
    // No default `completion` command.
    bool DisableDefaultCmd = false;
    // No --no-descriptions flag on the `completion` subcommands.
    bool DisableNoDescFlag = false;
    // No descriptions in completions.
    bool DisableDescriptions = false;
    // Hide the default `completion` command.
    bool HiddenDefaultCmd = false;
    // The directive for arguments without completions; inherited by
    // subcommands.
    std::optional<ShellCompDirective> DefaultShellCompDirective{};

    void SetDefaultShellCompDirective(ShellCompDirective directive) { DefaultShellCompDirective = directive; }
};

namespace detail {

struct commandState;
struct commandAccess;

// The state of a Command that isn't its fields: the reference count, the
// flags, the tree. A member, so that Command stays an aggregate.
class commandHandle {
public:
    commandHandle();
    // A copy is a new command: no parent, children or flags.
    commandHandle(const commandHandle&);
    commandHandle& operator=(const commandHandle&);
    ~commandHandle();

private:
    friend struct gopp::cmd::Command;
    friend struct commandAccess;
    mutable std::atomic<unsigned> refs_{0};
    std::unique_ptr<commandState> s_;
};

}  // namespace detail

// cobra.Command: a command, such as `git` or `git commit`. Fill in the
// fields, then add subcommands and flags.
struct Command {
    // The one-line usage: the command's name first, then what it takes
    // ("add [-F file | -D dir]... [-f format] profile").
    string Use{};
    // Other names the command may be called by.
    std::vector<string> Aliases{};
    // Names to suggest this command for, besides its own's look-alikes.
    std::vector<string> SuggestFor{};
    // A short description, for the parent's command list.
    string Short{};
    // The group, in the parent's Groups(), to list the command in.
    string GroupID{};
    // A long description, for `help <this-command>`.
    string Long{};
    // Examples of the command's use.
    string Example{};
    // The arguments shell completion offers when there's no
    // ValidArgsFunction (and OnlyValidArgs accepts).
    std::vector<Completion> ValidArgs{};
    // Completes the arguments; with ValidArgs, it isn't called.
    CompletionFunc ValidArgsFunction{};
    // Validates the arguments.
    PositionalArgs Args{};
    // More arguments shell completion offers, unlisted.
    std::vector<string> ArgAliases{};
    // Not used: for the legacy bash completion, which isn't ported.
    string BashCompletionFunction{};
    // If set, the command is deprecated (hidden), and this is printed when
    // it's used.
    string Deprecated{};
    // Key/value pairs for applications to identify or group commands.
    std::map<string, string> Annotations{};
    // Enables --version, which prints this (with VersionTemplate()).
    string Version{};

    // The hooks, run in this order: PersistentPreRun, PreRun, Run, PostRun,
    // PersistentPostRun. Persistent hooks are inherited by children (the
    // nearest one runs, or all with EnableTraverseRunHooks). The E forms
    // return errors and take precedence.
    RunFunc PersistentPreRun{};
    RunEFunc PersistentPreRunE{};
    RunFunc PreRun{};
    RunEFunc PreRunE{};
    // The command's work. A command without Run(E) only shows help.
    RunFunc Run{};
    RunEFunc RunE{};
    RunFunc PostRun{};
    RunEFunc PostRunE{};
    RunFunc PersistentPostRun{};
    RunEFunc PersistentPostRunE{};

    // Flag parsing errors to ignore.
    cmd::FParseErrWhitelist FParseErrWhitelist{};
    // Shell completion options.
    cmd::CompletionOptions CompletionOptions{};
    // Parse each command's flags while finding the subcommand, so that
    // parents' local flags can come before the subcommand.
    bool TraverseChildren = false;
    // Hidden from the command lists.
    bool Hidden = false;
    // Don't print errors.
    bool SilenceErrors = false;
    // Don't print the usage after errors.
    bool SilenceUsage = false;
    // All arguments go to the command, flags too.
    bool DisableFlagParsing = false;
    // No "Auto generated by spf13/cobra" line in generated docs.
    bool DisableAutoGenTag = false;
    // No "[flags]" in the usage line.
    bool DisableFlagsInUseLine = false;
    // No "did you mean" suggestions for unknown commands.
    bool DisableSuggestions = false;
    // The Levenshtein distance up to which to suggest; 2 if 0.
    int SuggestionsMinimumDistance = 0;

    // Internal state: flags, subcommands, settings; and the reference
    // count, for ptr<Command>.
    detail::commandHandle state_{};
    void addRef() const noexcept { state_.refs_.fetch_add(1, std::memory_order_relaxed); }
    void releaseRef() const noexcept;

    // --- Context, arguments and streams

    // The context ExecuteContext was given (context::Background() by
    // default), for Run functions.
    ptr<context::Context> Context();
    void SetContext(ptr<context::Context> ctx);
    // The arguments to parse instead of os::Args() (for tests).
    void SetArgs(std::vector<string> a);
    // Deprecated: SetOut and SetErr.
    void SetOutput(ptr<io::Writer> output);
    // Where usage, help and Print go; inherited by subcommands.
    void SetOut(ptr<io::Writer> newOut);
    // Where errors and PrintErr go; inherited by subcommands.
    void SetErr(ptr<io::Writer> newErr);
    // Where InOrStdin reads; inherited by subcommands.
    void SetIn(ptr<io::Reader> newIn);
    io::Writer& OutOrStdout() const;
    io::Writer& OutOrStderr() const;
    io::Writer& ErrOrStderr() const;
    io::Reader& InOrStdin() const;

    // --- Help, usage and errors

    // Replaces the usage function; inherited by subcommands.
    void SetUsageFunc(std::function<error(Command&)> f);
    // Replaces the usage template ("" restores the default).
    void SetUsageTemplate(string s);
    // Replaces what flag parsing errors go through.
    void SetFlagErrorFunc(std::function<error(Command&, error)> f);
    // Replaces the help function; inherited by subcommands.
    void SetHelpFunc(std::function<void(Command&, std::span<const string>)> f);
    // Replaces the help command.
    void SetHelpCommand(ptr<Command> cmd);
    // The group of the help command.
    void SetHelpCommandGroupID(string groupID);
    // The group of the completion command.
    void SetCompletionCommandGroupID(string groupID);
    // Replaces the help template ("" restores the default).
    void SetHelpTemplate(string s);
    // Replaces the version template ("" restores the default).
    void SetVersionTemplate(string s);
    // Replaces the "Error:" printed before errors.
    void SetErrPrefix(string s);
    // Sets a normalization function on all flags, of this command and its
    // subcommands.
    void SetGlobalNormalizationFunc(pflag::FlagSet::NormalizeFunc n);
    pflag::FlagSet::NormalizeFunc GlobalNormalizationFunc() const;

    std::function<error(Command&)> UsageFunc() const;
    // Prints the usage message.
    error Usage() const;
    std::function<void(Command&, std::span<const string>)> HelpFunc() const;
    // Prints the help message.
    error Help() const;
    // The usage message.
    string UsageString() const;
    std::function<error(Command&, error)> FlagErrorFunc() const;
    // Paddings for the default templates.
    int UsagePadding() const;
    int CommandPathPadding() const;
    int NamePadding() const;
    string UsageTemplate() const;
    string HelpTemplate() const;
    string VersionTemplate() const;
    string ErrPrefix() const;

    // --- Finding and running commands

    // The subcommand that args name, the remaining arguments, and an
    // error for unknown commands.
    std::tuple<Command*, std::vector<string>, error> Find(std::span<const string> args);
    // Find, parsing each command's flags on the way (TraverseChildren).
    std::tuple<Command*, std::vector<string>, error> Traverse(std::span<const string> args);
    // The names of subcommands that typedName may be a typo of.
    std::vector<string> SuggestionsFor(std::string_view typedName) const;
    // Calls fn with each ancestor, parent first.
    void VisitParents(const std::function<void(Command&)>& fn);
    Command* Root() const;
    Command* Parent() const;
    bool HasParent() const;
    // The number of arguments before "--", or -1.
    isize ArgsLenAtDash() const;

    // Runs the command line (os::Args(), or SetArgs's) from the root: finds
    // the subcommand, parses flags, validates and runs.
    error Execute();
    error ExecuteContext(ptr<context::Context> ctx);
    // Execute, also returning the command that ran.
    std::pair<Command*, error> ExecuteC();
    std::pair<Command*, error> ExecuteContextC(ptr<context::Context> ctx);

    // Runs Args (ArbitraryArgs if unset).
    error ValidateArgs(std::span<const string> args);
    // Fails if a required flag wasn't given.
    error ValidateRequiredFlags();
    // Fails if the flag groups' rules are broken.
    error ValidateFlagGroups();

    // Adds --help and -h (unless defined), --version if Version is set,
    // the help command if there are subcommands.
    void InitDefaultHelpFlag();
    void InitDefaultVersionFlag();
    void InitDefaultHelpCmd();
    // Adds the `completion` command, unless disabled or defined.
    void InitDefaultCompletionCmd(std::span<const string> args = {});

    // --- The tree

    // The subcommands; sorted by name if EnableCommandSorting.
    const std::vector<ptr<Command>>& Commands() const;
    template <typename... C>
        requires(std::convertible_to<C, ptr<Command>> && ...)
    void AddCommand(C&&... cmds) {
        const ptr<Command> list[] = {ptr<Command>(std::forward<C>(cmds))...};
        AddCommands(list);
    }
    void AddCommands(std::span<const ptr<Command>> cmds);
    template <typename... C>
        requires(std::convertible_to<C, ptr<Command>> && ...)
    void RemoveCommand(C&&... cmds) {
        const ptr<Command> list[] = {ptr<Command>(std::forward<C>(cmds))...};
        RemoveCommands(list);
    }
    void RemoveCommands(std::span<const ptr<Command>> cmds);
    // Removes the subcommands, help command and parent.
    void ResetCommands();
    const std::vector<Group>& Groups() const;
    template <typename... G>
        requires(std::convertible_to<G, Group> && ...)
    void AddGroup(G&&... groups) {
        const Group list[] = {Group(std::forward<G>(groups))...};
        AddGroups(list);
    }
    void AddGroups(std::span<const Group> groups);
    bool AllChildCommandsHaveGroup() const;
    bool ContainsGroup(std::string_view groupID) const;

    // --- Output: to OutOrStderr, or ErrOrStderr for PrintErr.

    void Print(std::string_view s) const;
    template <typename... A>
    void Printf(std::format_string<A...> f, A&&... a) const {
        Print(std::format(f, std::forward<A>(a)...));
    }
    template <typename... A>
    void Println(std::format_string<A...> f, A&&... a) const {
        Print(std::format(f, std::forward<A>(a)...) + "\n");
    }
    void PrintErr(std::string_view s) const;
    template <typename... A>
    void PrintErrf(std::format_string<A...> f, A&&... a) const {
        PrintErr(std::format(f, std::forward<A>(a)...));
    }
    template <typename... A>
    void PrintErrln(std::format_string<A...> f, A&&... a) const {
        PrintErr(std::format(f, std::forward<A>(a)...) + "\n");
    }

    // --- Names

    // The names from the root down: "git commit".
    string CommandPath() const;
    // The name, or the CommandDisplayNameAnnotation.
    string DisplayName() const;
    // The usage line: the parents' path and Use, with " [flags]".
    string UseLine() const;
    // Prints the flags of the command and its children (debugging).
    void DebugFlags();
    // The first word of Use.
    string Name() const;
    bool HasAlias(std::string_view s) const;
    // The name or alias the command was called by, if it ran.
    string CalledAs() const;
    // "name, alias1, alias2"
    string NameAndAliases() const;
    bool HasExample() const;
    // Whether it has Run or RunE.
    bool Runnable() const;
    bool HasSubCommands() const;
    // Not deprecated, hidden or help; runnable or with such subcommands.
    bool IsAvailableCommand() const;
    // Not runnable, deprecated or hidden, nor are its subcommands: help
    // text only.
    bool IsAdditionalHelpTopicCommand() const;
    bool HasHelpSubCommands() const;
    bool HasAvailableSubCommands() const;

    // --- Flags

    // All flags: local, persistent, and inherited after parsing.
    pflag::FlagSet* Flags() const;
    // The local flags that aren't persistent.
    ptr<pflag::FlagSet> LocalNonPersistentFlags() const;
    // The flags defined here, persistent or not.
    pflag::FlagSet* LocalFlags() const;
    // The flags inherited from the parents' persistent flags.
    pflag::FlagSet* InheritedFlags() const;
    pflag::FlagSet* NonInheritedFlags() const;
    // The flags that subcommands inherit too.
    pflag::FlagSet* PersistentFlags() const;
    // Removes all flags.
    void ResetFlags();
    bool HasFlags() const;
    bool HasPersistentFlags() const;
    bool HasLocalFlags() const;
    bool HasInheritedFlags() const;
    // The same, counting only flags that aren't hidden.
    bool HasAvailableFlags() const;
    bool HasAvailablePersistentFlags() const;
    bool HasAvailableLocalFlags() const;
    bool HasAvailableInheritedFlags() const;
    // The flag named name, local, persistent or inherited; nullptr if none.
    pflag::Flag* Flag(std::string_view name) const;
    // Parses args into the flags.
    error ParseFlags(std::span<const string> args);

    // Required flags.
    error MarkFlagRequired(std::string_view name);
    error MarkPersistentFlagRequired(std::string_view name);
    // File and directory completion of flags' values.
    template <typename... S>
        requires(std::convertible_to<const S&, std::string_view> && ...)
    error MarkFlagFilename(std::string_view name, const S&... extensions) {
        return markFlagFilename(Flags(), name, {string(std::string_view(extensions))...});
    }
    template <typename... S>
        requires(std::convertible_to<const S&, std::string_view> && ...)
    error MarkPersistentFlagFilename(std::string_view name, const S&... extensions) {
        return markFlagFilename(PersistentFlags(), name, {string(std::string_view(extensions))...});
    }
    error MarkFlagCustom(std::string_view name, std::string_view f);
    error MarkFlagDirname(std::string_view name);
    error MarkPersistentFlagDirname(std::string_view name);
    // Deprecated no-ops, as in cobra.
    error MarkZshCompPositionalArgumentFile(int argPosition, std::vector<string> patterns = {});
    error MarkZshCompPositionalArgumentWords(int argPosition, std::vector<string> words = {});

    // Flag groups: all or none of the flags; at least one of them; at
    // most one of them.
    template <typename... S>
        requires(std::convertible_to<const S&, std::string_view> && ...)
    void MarkFlagsRequiredTogether(const S&... flagNames) {
        markFlagGroup(0, {string(std::string_view(flagNames))...});
    }
    template <typename... S>
        requires(std::convertible_to<const S&, std::string_view> && ...)
    void MarkFlagsOneRequired(const S&... flagNames) {
        markFlagGroup(1, {string(std::string_view(flagNames))...});
    }
    template <typename... S>
        requires(std::convertible_to<const S&, std::string_view> && ...)
    void MarkFlagsMutuallyExclusive(const S&... flagNames) {
        markFlagGroup(2, {string(std::string_view(flagNames))...});
    }
    // The same with a list of names (Go's f(names...)).
    void MarkFlagsRequiredTogether(std::span<const string> flagNames) { markFlagGroup(0, {flagNames.begin(), flagNames.end()}); }
    void MarkFlagsOneRequired(std::span<const string> flagNames) { markFlagGroup(1, {flagNames.begin(), flagNames.end()}); }
    void MarkFlagsMutuallyExclusive(std::span<const string> flagNames) { markFlagGroup(2, {flagNames.begin(), flagNames.end()}); }

    // --- Shell completion

    // Completes the values of a flag.
    error RegisterFlagCompletionFunc(std::string_view flagName, CompletionFunc f);
    std::pair<CompletionFunc, bool> GetFlagCompletionFunc(std::string_view flagName) const;

    // Completion scripts. The shells run `<program> __complete <args>`,
    // which these programs answer.
    error GenBashCompletionV2(io::Writer& w, bool includeDesc);
    error GenBashCompletionFileV2(std::string_view filename, bool includeDesc);
    error GenZshCompletion(io::Writer& w);
    error GenZshCompletionNoDesc(io::Writer& w);
    error GenZshCompletionFile(std::string_view filename);
    error GenZshCompletionFileNoDesc(std::string_view filename);
    error GenFishCompletion(io::Writer& w, bool includeDesc);
    error GenFishCompletionFile(std::string_view filename, bool includeDesc);
    error GenPowerShellCompletion(io::Writer& w);
    error GenPowerShellCompletionWithDesc(io::Writer& w);
    error GenPowerShellCompletionFile(std::string_view filename);
    error GenPowerShellCompletionFileWithDesc(std::string_view filename);

private:
    friend struct detail::commandAccess;
    error markFlagFilename(pflag::FlagSet* flags, std::string_view name, std::vector<string> extensions);
    void markFlagGroup(int kind, std::vector<string> flagNames);
};

// Allocates a command: &cobra.Command{...}.
ptr<Command> New(Command c);

// --- Settings, as cobra's package variables.

// Allow unambiguous prefixes of command names ("conf" for "config").
extern bool EnablePrefixMatching;
// Sort Commands() by name (the default).
extern bool EnableCommandSorting;
// Match command names case-insensitively.
extern bool EnableCaseInsensitive;
// Run all ancestors' persistent hooks, not only the nearest one's.
extern bool EnableTraverseRunHooks;

// Functions run before each command runs (after flags are parsed), and
// after.
void OnInitialize(std::function<void()> f);
void OnFinalize(std::function<void()> f);

// Functions for usage and help templates, besides cobra's (trim,
// trimRightSpace, trimTrailingWhitespaces, appendIfNotPresent, rpad, gt,
// eq).
void AddTemplateFunc(std::string name, text::tmpl::Func f);
void AddTemplateFuncs(const text::tmpl::FuncMap& funcs);

// Prints "Error: msg" and exits with status 1 if err isn't nil.
void CheckErr(const error& err);
// Writes s to w, CheckErr-ing the result.
void WriteStringAndCheck(io::Writer& w, std::string_view s);

// --- Positional argument validators.

// Fails on any argument (an unknown subcommand, presumably).
error NoArgs(Command& cmd, std::span<const string> args);
// Fails on arguments not in ValidArgs.
error OnlyValidArgs(Command& cmd, std::span<const string> args);
// Accepts anything.
error ArbitraryArgs(Command& cmd, std::span<const string> args);
PositionalArgs MinimumNArgs(int n);
PositionalArgs MaximumNArgs(int n);
PositionalArgs ExactArgs(int n);
PositionalArgs RangeArgs(int min, int max);
// All of the validators, in order.
PositionalArgs MatchAll(std::vector<PositionalArgs> pargs);
template <typename... P>
    requires(std::convertible_to<P, PositionalArgs> && ...)
PositionalArgs MatchAll(P&&... pargs) {
    return MatchAll(std::vector<PositionalArgs>{PositionalArgs(std::forward<P>(pargs))...});
}
// Deprecated: MatchAll(ExactArgs(n), OnlyValidArgs).
PositionalArgs ExactValidArgs(int n);

// --- Completion helpers.

// "choice\tdescription"
Completion CompletionWithDesc(std::string_view choice, std::string_view description);
// A CompletionFunc offering nothing, not even files.
std::pair<std::vector<Completion>, ShellCompDirective> NoFileCompletions(Command& cmd, std::span<const string> args,
                                                                         std::string_view toComplete);
// A CompletionFunc offering choices.
CompletionFunc FixedCompletions(std::vector<Completion> choices, ShellCompDirective directive);
// Debugging output of completion code: to $BASH_COMP_DEBUG_FILE, and to
// standard error if printToStdErr.
void CompDebug(std::string_view msg, bool printToStdErr);
void CompDebugln(std::string_view msg, bool printToStdErr);
// An error from completion code, which the shell shows.
void CompError(std::string_view msg);
void CompErrorln(std::string_view msg);
// Adds a message to show the user while completing (active help).
std::vector<Completion> AppendActiveHelp(std::vector<Completion> compArray, std::string_view activeHelpStr);
// The active help setting: $<PROGRAM>_ACTIVE_HELP, or "0" if
// $COBRA_ACTIVE_HELP is "0".
string GetActiveHelpConfig(const Command& cmd);

// The annotations of flags for completion and required flags.
error MarkFlagRequired(pflag::FlagSet* flags, std::string_view name);
error MarkFlagFilename(pflag::FlagSet* flags, std::string_view name, std::vector<string> extensions = {});
error MarkFlagCustom(pflag::FlagSet* flags, std::string_view name, std::string_view f);
error MarkFlagDirname(pflag::FlagSet* flags, std::string_view name);

}  // namespace gopp::cmd
