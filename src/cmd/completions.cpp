// After spf13/cobra v1.10.2's completions.go, active_help.go and
// shell_completions.go.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include "cmd/internal.hpp"

#include <map>
#include <string>

namespace gopp::cmd {

using detail::S;

namespace {

constexpr std::string_view compCmdName = "completion";
constexpr std::string_view compCmdNoDescFlagName = "no-descriptions";
constexpr std::string_view compCmdNoDescFlagDesc = "disable completion descriptions";
constexpr bool compCmdNoDescFlagDefault = false;

constexpr std::string_view configEnvVarGlobalPrefix = "COBRA";
constexpr std::string_view configEnvVarSuffixDescriptions = "COMPLETION_DESCRIPTIONS";
constexpr std::string_view activeHelpEnvVarSuffix = "ACTIVE_HELP";
constexpr std::string_view activeHelpGlobalEnvVar = "COBRA_ACTIVE_HELP";
constexpr std::string_view activeHelpGlobalDisable = "0";

constexpr int shellCompDirectiveMaxValue = 1 << 6;

// The completion functions of flags, as cobra's flagCompletionFunctions.
// Entries are dropped when the command that registered them goes.
struct flagCompletions {
    sync::RWMutex mu;
    std::map<const pflag::Flag*, CompletionFunc> fns;
};

flagCompletions& flagCompletionFunctions() {
    static auto* f = new flagCompletions();
    return *f;
}

CompletionFunc lookupFlagCompletion(const pflag::Flag* flag) {
    auto& fc = flagCompletionFunctions();
    fc.mu.RLock();
    CompletionFunc fn;
    if (auto it = fc.fns.find(flag); it != fc.fns.end()) fn = it->second;
    fc.mu.RUnlock();
    return fn;
}

// The error of completing a flag the command doesn't have.
struct flagCompError final : errorInterface {
    string subCommand;
    string flagName;
    flagCompError(string s, string f) : subCommand(std::move(s)), flagName(std::move(f)) {}
    string Error() const override { return "Subcommand '"_s + subCommand + "' does not support flag '" + flagName + "'"; }
};

string directiveString(ShellCompDirective d) {
    std::vector<string> directives;
    if (d & ShellCompDirectiveError) directives.push_back("ShellCompDirectiveError"_s);
    if (d & ShellCompDirectiveNoSpace) directives.push_back("ShellCompDirectiveNoSpace"_s);
    if (d & ShellCompDirectiveNoFileComp) directives.push_back("ShellCompDirectiveNoFileComp"_s);
    if (d & ShellCompDirectiveFilterFileExt) directives.push_back("ShellCompDirectiveFilterFileExt"_s);
    if (d & ShellCompDirectiveFilterDirs) directives.push_back("ShellCompDirectiveFilterDirs"_s);
    if (d & ShellCompDirectiveKeepOrder) directives.push_back("ShellCompDirectiveKeepOrder"_s);
    if (directives.empty()) directives.push_back("ShellCompDirectiveDefault"_s);
    if (static_cast<int>(d) >= shellCompDirectiveMaxValue) {
        return fmt::Sprint("ERROR: unexpected ShellCompDirective value: {}", static_cast<int>(d));
    }
    return strings::Join(directives, ", ");
}

bool nonCompletableFlag(const pflag::Flag* flag) { return flag->Hidden || !flag->Deprecated.empty(); }

std::vector<Completion> getFlagNameCompletions(const pflag::Flag* flag, std::string_view toComplete) {
    if (nonCompletableFlag(flag)) return {};
    std::vector<Completion> completions;
    string flagName = "--"_s + flag->Name;
    if (flagName.starts_with(toComplete)) {
        // Flag without the =
        completions.push_back(CompletionWithDesc(flagName, flag->Usage));
    }
    flagName = "-"_s + flag->Shorthand;
    if (!flag->Shorthand.empty() && flagName.starts_with(toComplete)) completions.push_back(CompletionWithDesc(flagName, flag->Usage));
    return completions;
}

std::vector<Completion> completeRequireFlags(Command& finalCmd, std::string_view toComplete) {
    std::vector<Completion> completions;
    auto doCompleteRequiredFlags = [&](pflag::Flag* flag) {
        if (flag->Annotations.contains(string::fromStatic(BashCompOneRequiredFlag)) && !flag->Changed) {
            // If the flag is not already present, we suggest it as a
            // completion.
            for (auto& c : getFlagNameCompletions(flag, toComplete)) completions.push_back(c);
        }
    };
    // We cannot use finalCmd.Flags() because we may not have called
    // ParsedFlags() for commands that have DisableFlagParsing==true.
    finalCmd.InheritedFlags()->VisitAll(doCompleteRequiredFlags);
    finalCmd.NonInheritedFlags()->VisitAll(doCompleteRequiredFlags);
    return completions;
}

pflag::Flag* findFlag(Command& cmd, string name) {
    pflag::FlagSet* flagSet = cmd.Flags();
    if (name.size() == 1) {
        // First convert the short flag into a long flag as the cmd.Flag()
        // search only accepts long flags.
        if (pflag::Flag* sh = flagSet->ShorthandLookup(name.view())) {
            name = sh->Name;
        } else {
            pflag::FlagSet* set = cmd.InheritedFlags();
            if ((sh = set->ShorthandLookup(name.view()))) name = sh->Name;
            else return nullptr;
        }
    }
    return cmd.Flag(name.view());
}

struct flagCheck {
    pflag::Flag* flag;
    std::vector<string> args;
    string lastArg;
    error err;
};

flagCheck checkIfFlagCompletion(Command& finalCmd, std::vector<string> args, string lastArg) {
    if (finalCmd.DisableFlagParsing) {
        // We only do flag completion if we are allowed to parse flags.
        // This is important for commands which have requested to do their
        // own flag completion.
        return {nullptr, std::move(args), std::move(lastArg), nullptr};
    }
    string flagName;
    std::vector<string> trimmedArgs = args;
    bool flagWithEqual = false;
    string orgLastArg = lastArg;

    // When doing completion of a flag name, as soon as an argument starts
    // with a '-' we know it is a flag. We cannot use isFlagArg() here as
    // that function requires the flag name to be complete (e.g., -- does
    // not pass the check).
    if (!lastArg.empty() && lastArg[0] == '-') {
        if (auto index = lastArg.find('='); index != string::npos) {
            // Flag with an =
            if (lastArg.slice(0, index).starts_with("--")) {
                // Flag has full name
                flagName = lastArg.slice(2, index);
            } else {
                // Flag is shorthand. We have to get the last shorthand
                // flag name, e.g. `-asd` => d to provide the correct
                // completion: https://github.com/spf13/cobra/issues/1257
                flagName = lastArg.slice(index - 1, index);
            }
            lastArg = lastArg.slice(index + 1);
            flagWithEqual = true;
        } else {
            // Normal flag completion.
            return {nullptr, std::move(args), std::move(lastArg), nullptr};
        }
    }

    if (flagName.empty()) {
        if (!args.empty()) {
            const string& prevArg = args.back();
            if (detail::isFlagArg(prevArg.view())) {
                // Only consider the case where the flag does not contain
                // an =. If the flag contains an = it means it has already
                // been fully processed, so we don't need to deal with it
                // here.
                if (prevArg.find('=') == string::npos) {
                    if (prevArg.starts_with("--")) {
                        // Flag has full name.
                        flagName = prevArg.slice(2);
                    } else {
                        // Flag is shorthand. We have to get the last
                        // shorthand flag name, e.g. `-asd` => d.
                        flagName = prevArg.slice(prevArg.size() - 1);
                    }
                    // Remove the uncompleted flag or else there could be
                    // an error created for an invalid value for that flag.
                    trimmedArgs.pop_back();
                }
            }
        }
    }

    if (flagName.empty()) {
        // Not doing flag completion.
        return {nullptr, std::move(trimmedArgs), std::move(lastArg), nullptr};
    }

    pflag::Flag* flag = findFlag(finalCmd, flagName);
    if (!flag) {
        // Flag not supported by this command, the interspersed option
        // might be set so return the original args.
        return {nullptr, std::move(args), std::move(orgLastArg), make_ptr<flagCompError>(finalCmd.Name(), flagName)};
    }
    if (!flagWithEqual) {
        if (!flag->NoOptDefVal.empty()) {
            // We had assumed dealing with a two-word flag but the flag is
            // a boolean flag. In that case, there is no value following
            // it, so we are not really doing flag completion. Reset
            // everything to do noun completion.
            trimmedArgs = args;
            flag = nullptr;
        }
    }
    return {flag, std::move(trimmedArgs), std::move(lastArg), nullptr};
}

bool helpOrVersionFlagPresent(Command& cmd) {
    auto setByCobra = string::fromStatic(FlagSetByCobraAnnotation);
    if (pflag::Flag* versionFlag = cmd.Flags()->Lookup("version")) {
        auto it = versionFlag->Annotations.find(setByCobra);
        if (it != versionFlag->Annotations.end() && !it->second.empty() && versionFlag->Changed) return true;
    }
    if (pflag::Flag* helpFlag = cmd.Flags()->Lookup("help")) {
        auto it = helpFlag->Annotations.find(setByCobra);
        if (it != helpFlag->Annotations.end() && !it->second.empty() && helpFlag->Changed) return true;
    }
    return false;
}

struct completionResult {
    Command* cmd;
    std::vector<Completion> completions;
    ShellCompDirective directive;
    error err;
};

completionResult getCompletions(Command& c, std::span<const string> args) {
    // The last argument, which is not completely typed by the user, should
    // not be part of the list of arguments.
    string toComplete = args.back();
    std::vector<string> trimmedArgs(args.begin(), args.end() - 1);

    Command* finalCmd = nullptr;
    std::vector<string> finalArgs;
    error err;
    // Find the real command for which completion must be performed. Check
    // if we need to traverse here to parse local flags on parent commands.
    if (c.Root()->TraverseChildren) {
        std::tie(finalCmd, finalArgs, err) = c.Root()->Traverse(trimmedArgs);
    } else {
        // For Root commands that don't specify any value for their Args
        // fields, when we call Find(), if those Root commands don't have
        // any sub-commands, they will accept arguments. However, because
        // we have added the __complete sub-command in the current code
        // path, the call to Find() -> legacyArgs() will return an error if
        // there are any arguments. To avoid this, we first remove the
        // __complete command to get back to having no sub-commands.
        Command* rootCmd = c.Root();
        if (rootCmd->Commands().size() == 1) {
            ptr<Command> self(&c);
            S(*rootCmd).retired.push_back(self);
            rootCmd->RemoveCommand(self);
        }
        std::tie(finalCmd, finalArgs, err) = rootCmd->Find(trimmedArgs);
    }
    if (err) {
        // Unable to find the real command. E.g., <program> someInvalidCmd <TAB>
        std::string list = "[";
        for (std::size_t i = 0; i < trimmedArgs.size(); i++) {
            if (i > 0) list += ' ';
            list += trimmedArgs[i].view();
        }
        return {&c, {}, ShellCompDirectiveDefault, fmt::Errorf("unable to find a command for arguments: {}", list + "]")};
    }
    S(*finalCmd).ctx = S(c).ctx;

    // These flags are normally added when `execute()` is called on
    // `finalCmd`, however, when doing completion, we don't call
    // `finalCmd.execute()`. Let's add the --help and --version flag
    // ourselves but only if the finalCmd has not disabled flag parsing; if
    // flag parsing is disabled, it is up to the finalCmd itself to handle
    // the completion of *all* flags.
    if (!finalCmd->DisableFlagParsing) {
        finalCmd->InitDefaultHelpFlag();
        finalCmd->InitDefaultVersionFlag();
    }

    // Check if we are doing flag value completion before parsing the flags.
    // This is important because if we are completing a flag value, we
    // need to also remove the flag name argument from the list of
    // finalArgs or else the parsing could fail due to an invalid value
    // (incomplete) for the flag.
    auto check = checkIfFlagCompletion(*finalCmd, std::move(finalArgs), toComplete);
    pflag::Flag* flag = check.flag;
    finalArgs = std::move(check.args);
    toComplete = std::move(check.lastArg);
    error flagErr = check.err;

    // Check if interspersed is false or -- was set on a previous arg. This
    // works by counting the arguments. Normally -- is not counted as arg
    // but if -- was already set or interspersed is false and there is
    // already one arg then the extra added -- is counted as arg.
    bool flagCompletion = true;
    {
        std::vector<string> withDash = finalArgs;
        withDash.push_back("--"_s);
        finalCmd->ParseFlags(withDash);
    }
    isize newArgCount = finalCmd->Flags()->NArg();

    // Parse the flags early so we can check if required flags are set.
    if (auto perr = finalCmd->ParseFlags(finalArgs)) {
        std::string list = "[";
        for (std::size_t i = 0; i < finalArgs.size(); i++) {
            if (i > 0) list += ' ';
            list += finalArgs[i].view();
        }
        return {finalCmd, {}, ShellCompDirectiveDefault,
                fmt::Errorf("Error while parsing flags from args {}: {}", list + "]", perr.Error())};
    }

    isize realArgCount = finalCmd->Flags()->NArg();
    if (newArgCount > realArgCount) {
        // Don't do flag completion (see above).
        flagCompletion = false;
    }
    // Error while attempting to parse flags.
    if (flagErr) {
        // If error type is flagCompError and we don't want flagCompletion
        // we should ignore the error.
        if (!errors::As<flagCompError>(flagErr) || flagCompletion) return {finalCmd, {}, ShellCompDirectiveDefault, flagErr};
    }

    // Look for the --help or --version flags. If they are present, there
    // should be no further completions.
    if (helpOrVersionFlagPresent(*finalCmd)) return {finalCmd, {}, ShellCompDirectiveNoFileComp, nullptr};

    // We only remove the flags from the arguments if DisableFlagParsing is
    // not set. This is important for commands which have requested to do
    // their own flag completion.
    if (!finalCmd->DisableFlagParsing) finalArgs = finalCmd->Flags()->Args();

    if (flag && flagCompletion) {
        // Check if we are completing a flag value subject to annotations.
        if (auto it = flag->Annotations.find(string::fromStatic(BashCompFilenameExt)); it != flag->Annotations.end()) {
            if (!it->second.empty()) {
                // File completion filtered by extensions.
                return {finalCmd, it->second, ShellCompDirectiveFilterFileExt, nullptr};
            }
            // The annotation requests simple file completion. There is no
            // reason to do that since it is the default behavior anyway.
            // Let's ignore this annotation in case the program also
            // registered a completion function for this flag. Even though
            // it is a mistake on the program's side, let's be nice when we
            // can.
        }
        if (auto it = flag->Annotations.find(string::fromStatic(BashCompSubdirsInDir)); it != flag->Annotations.end()) {
            if (it->second.size() == 1) {
                // Directory completion from within a directory.
                return {finalCmd, it->second, ShellCompDirectiveFilterDirs, nullptr};
            }
            // Directory completion.
            return {finalCmd, {}, ShellCompDirectiveFilterDirs, nullptr};
        }
    }

    std::vector<Completion> completions;
    ShellCompDirective directive = ShellCompDirectiveDefault;

    // Enforce flag groups before doing flag completions.
    detail::enforceFlagGroupsForCompletion(*finalCmd);

    // Note that we want to perform flagname completion even if
    // finalCmd.DisableFlagParsing==true; finalCmd.DisableFlagParsing
    // means completion is handled for flag values, not for flag names.
    if (!flag && !toComplete.empty() && toComplete[0] == '-' && toComplete.find('=') == string::npos && flagCompletion) {
        // First check for required flags.
        completions = completeRequireFlags(*finalCmd, toComplete.view());
        // If we have not found any required flags, only then can we show
        // regular flags.
        if (completions.empty()) {
            auto doCompleteFlags = [&](pflag::Flag* f) {
                bool acceptsMultiple = dynamic_cast<pflag::SliceValue*>(f->Value.get()) != nullptr;
                string t = f->Value->Type();
                acceptsMultiple = acceptsMultiple || strings::Contains(t, "Slice") || strings::Contains(t, "Array") ||
                                  t.starts_with("stringTo");
                if (!f->Changed || acceptsMultiple) {
                    // If the flag is not already present, or if it can be
                    // specified multiple times (Array, Slice, or
                    // stringTo), we suggest it as a completion.
                    for (auto& comp : getFlagNameCompletions(f, toComplete.view())) completions.push_back(comp);
                }
            };
            // We cannot use finalCmd.Flags() because we may not have
            // called ParsedFlags() for commands that have
            // DisableFlagParsing==true.
            finalCmd->InheritedFlags()->VisitAll(doCompleteFlags);
            finalCmd->NonInheritedFlags()->VisitAll(doCompleteFlags);
        }
        directive = ShellCompDirectiveNoFileComp;
        if (completions.size() == 1 && completions[0].ends_with("=")) {
            // If there is a single completion, the shell usually adds a
            // space after the completion. We don't want that if the flag
            // ends with an =.
            directive = ShellCompDirectiveNoSpace;
        }
        if (!finalCmd->DisableFlagParsing) {
            // If DisableFlagParsing==false, we have completed the flags as
            // known by Cobra; we can return what we found. If
            // DisableFlagParsing==true, Cobra may not be aware of all
            // flags, so we let the logic continue to see if
            // ValidArgsFunction needs to be called.
            return {finalCmd, completions, directive, nullptr};
        }
    } else {
        directive = ShellCompDirectiveDefault;
        // Check if a default directive is set on the command or its
        // parents.
        for (Command* cmd = finalCmd; cmd; cmd = cmd->Parent()) {
            if (cmd->CompletionOptions.DefaultShellCompDirective) {
                directive = *cmd->CompletionOptions.DefaultShellCompDirective;
                break;
            }
        }
        if (!flag) {
            bool foundLocalNonPersistentFlag = false;
            // If TraverseChildren is true on the root command we don't
            // check for local flags because we can use a local flag on a
            // parent command.
            if (!finalCmd->Root()->TraverseChildren) {
                // Check if there are any local, non-persistent flags on
                // the command-line.
                auto localNonPersistentFlags = finalCmd->LocalNonPersistentFlags();
                finalCmd->NonInheritedFlags()->VisitAll([&](pflag::Flag* f) {
                    if (localNonPersistentFlags->Lookup(f->Name.view()) && f->Changed) foundLocalNonPersistentFlag = true;
                });
            }
            // Complete subcommand names, including the help command.
            if (finalArgs.empty() && !foundLocalNonPersistentFlag) {
                // We only complete sub-commands if: - there are no
                // arguments on the command-line and - there are no local,
                // non-persistent flags on the command-line or
                // TraverseChildren is true.
                for (auto& subCmd : finalCmd->Commands()) {
                    if (subCmd->IsAvailableCommand() || subCmd == S(*finalCmd).helpCommand) {
                        if (subCmd->Name().starts_with(toComplete.view())) {
                            completions.push_back(CompletionWithDesc(subCmd->Name(), subCmd->Short));
                        }
                        directive = ShellCompDirectiveNoFileComp;
                    }
                }
            }
            // Complete required flags even without the '-' prefix.
            for (auto& comp : completeRequireFlags(*finalCmd, toComplete.view())) completions.push_back(comp);
            // Always complete ValidArgs, even if we are completing a
            // subcommand name. This is for commands that have both
            // subcommands and ValidArgs.
            if (!finalCmd->ValidArgs.empty()) {
                if (finalArgs.empty()) {
                    // ValidArgs are only for the first argument.
                    for (auto& validArg : finalCmd->ValidArgs) {
                        if (validArg.starts_with(toComplete.view())) completions.push_back(validArg);
                    }
                    directive = ShellCompDirectiveNoFileComp;
                    // If no completions were found within commands or
                    // ValidArgs, see if there are any ArgAliases that
                    // should be completed.
                    if (completions.empty()) {
                        for (auto& argAlias : finalCmd->ArgAliases) {
                            if (argAlias.starts_with(toComplete.view())) completions.push_back(argAlias);
                        }
                    }
                }
                // If there are ValidArgs specified (even if they don't
                // match), we stop completion. Only one of ValidArgs or
                // ValidArgsFunction can be used for a single command.
                return {finalCmd, completions, directive, nullptr};
            }
            // Let the logic continue so as to add any ValidArgsFunction
            // completions, even if we already found sub-commands. This is
            // for commands that have subcommands but also specify a
            // ValidArgsFunction.
        }
    }

    // Find the completion function for the flag or command.
    CompletionFunc completionFn;
    if (flag && flagCompletion) completionFn = lookupFlagCompletion(flag);
    else completionFn = finalCmd->ValidArgsFunction;
    if (completionFn) {
        // Go custom completion defined for this flag or command. Call the
        // registered completion function to get the completions.
        auto [comps, d] = completionFn(*finalCmd, finalArgs, toComplete.view());
        directive = d;
        for (auto& comp : comps) completions.push_back(comp);
    }
    return {finalCmd, completions, directive, nullptr};
}

}  // namespace

namespace detail {

void forgetFlagCompletions(commandState& s) {
    if (s.completionFlags.empty()) return;
    auto& fc = flagCompletionFunctions();
    fc.mu.Lock();
    for (auto& f : s.completionFlags) fc.fns.erase(f.get());
    fc.mu.Unlock();
}

string configEnvVar(std::string_view name, std::string_view suffix) {
    // This format should not be changed: users will be using it
    // explicitly.
    std::string v = strings::ToUpper(fmt::Sprint("{}_{}", name, suffix)).str();
    for (char& ch : v) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) ch = '_';
    }
    return string(v);
}

string getEnvConfig(const Command& cmd, std::string_view suffix) {
    string v = os::Getenv(configEnvVar(cmd.Root()->Name().view(), suffix).view());
    if (v.empty()) v = os::Getenv(configEnvVar(configEnvVarGlobalPrefix, suffix).view());
    return v;
}

string activeHelpEnvVar(std::string_view name) { return configEnvVar(name, activeHelpEnvVarSuffix); }

void initCompleteCmd(Command& c, std::span<const string> args) {
    auto completeCmd = New({
        .Use = fmt::Sprint("{} [command-line]", ShellCompRequestCmd),
        .Aliases = {string::fromStatic(ShellCompNoDescRequestCmd)},
        .Short = "Request shell completion choices for the specified command-line"_s,
        .Long = fmt::Sprint("{} is a special command that is used by the shell completion logic\n{}", ShellCompRequestCmd,
                            "to request completion choices for the specified command-line."),
        .Args = MinimumNArgs(1),
        .Run =
            [](Command& cmd, std::span<const string> args) {
                auto [finalCmd, completions, directive, err] = getCompletions(cmd, args);
                if (err) {
                    CompErrorln(err.Error().view());
                    // Keep going for multiple reasons: 1- There could be
                    // some valid completions even though there was an
                    // error 2- Even without completions, we need to print
                    // the directive.
                }
                bool noDescriptions = cmd.CalledAs() == ShellCompNoDescRequestCmd;
                if (!noDescriptions) {
                    auto [doDescriptions, perr] = strconv::ParseBool(getEnvConfig(cmd, configEnvVarSuffixDescriptions));
                    if (!perr) noDescriptions = !doDescriptions;
                }
                bool noActiveHelp = GetActiveHelpConfig(*finalCmd) == activeHelpGlobalDisable;
                io::Writer& out = finalCmd->OutOrStdout();
                for (auto comp : completions) {
                    if (noActiveHelp && comp.starts_with(activeHelpMarker)) {
                        // Remove all activeHelp entries if it's disabled.
                        continue;
                    }
                    if (noDescriptions) {
                        // Remove any description that may be included
                        // following a tab character.
                        if (auto tab = comp.find('\t'); tab != string::npos) comp = comp.slice(0, tab);
                    }
                    // Make sure we only write the first line to the
                    // output. This is needed if a description contains a
                    // linebreak. Otherwise the shell scripts will
                    // interpret the other lines as new flags and could
                    // therefore provide a wrong completion.
                    if (auto nl = comp.find('\n'); nl != string::npos) comp = comp.slice(0, nl);
                    // Finally trim the completion. This is especially
                    // important to get rid of a trailing tab when there
                    // are no description following it. For example, a
                    // sub-command without a description should not be
                    // completed to "subcmd\t" but to "subcmd" instead.
                    comp = strings::TrimSpace(comp);
                    // Print each possible completion to the output for
                    // the completion script to consume.
                    fmt::Println(out, "{}", comp);
                }
                // As the last printout, print the completion directive
                // for the completion script to parse. The directive
                // integer must be that last character following a single
                // colon (:). The completion script expects
                // :<directive>
                fmt::Print(out, ":{}\n", static_cast<int>(directive));
                // Print some helpful info to stderr for the user to
                // understand. Output from stderr must be ignored by the
                // completion script.
                fmt::Print(finalCmd->ErrOrStderr(), "Completion ended with directive: {}\n", directiveString(directive));
            },
        .Hidden = true,
        .DisableFlagParsing = true,
        .DisableFlagsInUseLine = true,
    });
    c.AddCommand(completeCmd);
    auto [subCmd, _, err] = c.Find(args);
    if (err || subCmd->Name() != ShellCompRequestCmd) {
        // Only create this special command if it is actually being called.
        // This reduces possible side-effects of creating such a command;
        // for example, having this command would cause problems to a cobra
        // program that only consists of the root command, since this
        // command would cause the root command to suddenly have a
        // subcommand.
        c.RemoveCommand(completeCmd);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API

Completion CompletionWithDesc(std::string_view choice, std::string_view description) {
    return string(std::string(choice) + "\t" + std::string(description));
}

std::pair<std::vector<Completion>, ShellCompDirective> NoFileCompletions(Command&, std::span<const string>, std::string_view) {
    return {{}, ShellCompDirectiveNoFileComp};
}

CompletionFunc FixedCompletions(std::vector<Completion> choices, ShellCompDirective directive) {
    return [choices = std::move(choices), directive](Command&, std::span<const string>, std::string_view) {
        return std::pair<std::vector<Completion>, ShellCompDirective>{choices, directive};
    };
}

error Command::RegisterFlagCompletionFunc(std::string_view flagName, CompletionFunc f) {
    pflag::Flag* flag = Flag(flagName);
    if (!flag) return fmt::Errorf("RegisterFlagCompletionFunc: flag '{}' does not exist", flagName);
    auto& fc = flagCompletionFunctions();
    fc.mu.Lock();
    if (fc.fns.contains(flag)) {
        fc.mu.Unlock();
        return fmt::Errorf("RegisterFlagCompletionFunc: flag '{}' already registered", flagName);
    }
    fc.fns[flag] = std::move(f);
    fc.mu.Unlock();
    S(*this).completionFlags.push_back(ptr<pflag::Flag>(flag));
    return nullptr;
}

std::pair<CompletionFunc, bool> Command::GetFlagCompletionFunc(std::string_view flagName) const {
    pflag::Flag* flag = Flag(flagName);
    if (!flag) return {nullptr, false};
    auto& fc = flagCompletionFunctions();
    fc.mu.RLock();
    auto it = fc.fns.find(flag);
    std::pair<CompletionFunc, bool> r{it == fc.fns.end() ? CompletionFunc() : it->second, it != fc.fns.end()};
    fc.mu.RUnlock();
    return r;
}

void Command::InitDefaultCompletionCmd(std::span<const string> args) {
    if (CompletionOptions.DisableDefaultCmd) return;
    auto& s = S(*this);
    for (auto& cmd : s.commands) {
        if (cmd->Name() == compCmdName || cmd->HasAlias(compCmdName)) {
            // A completion command is already available.
            return;
        }
    }
    bool haveNoDescFlag = !CompletionOptions.DisableNoDescFlag && !CompletionOptions.DisableDescriptions;

    // Special case to know if there are sub-commands or not.
    bool hasSubCommands = false;
    for (auto& cmd : s.commands) {
        if (cmd->Name() != ShellCompRequestCmd && cmd->Name() != "help") {
            hasSubCommands = true;
            break;
        }
    }

    string rootName = Root()->Name();
    auto completionCmd = New({
        .Use = string::fromStatic(compCmdName),
        .Short = "Generate the autocompletion script for the specified shell"_s,
        .Long = fmt::Sprint("Generate the autocompletion script for {} for the specified shell.\n"
                            "See each sub-command's help for details on how to use the generated script.\n",
                            rootName),
        .ValidArgsFunction = NoFileCompletions,
        .Args = NoArgs,
        .Hidden = CompletionOptions.HiddenDefaultCmd,
    });
    completionCmd->GroupID = s.completionCommandGroupID;
    AddCommand(completionCmd);

    if (!hasSubCommands) {
        // If the 'completion' command will be the only sub-command, we
        // only create it if it is actually being called. This avoids
        // breaking programs that would suddenly find themselves with a
        // subcommand, which would prevent them from accepting arguments.
        // We also create the 'completion' command if the user is
        // triggering shell completion for it (prog __complete
        // completion '').
        auto [subCmd, cmdArgs, err] = Find(args);
        if (err || (subCmd->Name() != compCmdName &&
                    (subCmd->Name() != ShellCompRequestCmd || cmdArgs.size() <= 1 || cmdArgs[0] != compCmdName))) {
            // The completion command is not being called or being
            // completed so we remove it.
            RemoveCommand(completionCmd);
            return;
        }
    }

    // The output and the --no-descriptions setting, which the subcommands
    // share (Go's closures capture them).
    io::Writer* out = &OutOrStdout();
    auto noDesc = std::make_shared<bool>(CompletionOptions.DisableDescriptions);

    auto bash = New({
        .Use = "bash"_s,
        .Short = fmt::Sprint("Generate the autocompletion script for {}", "bash"),
        .Long = fmt::Sprint(R"(Generate the autocompletion script for the bash shell.

This script depends on the 'bash-completion' package.
If it is not installed already, you can install it via your OS's package manager.

To load completions in your current shell session:

	source <({0} completion bash)

To load completions for every new session, execute once:

#### Linux:

	{0} completion bash > /etc/bash_completion.d/{0}

#### macOS:

	{0} completion bash > $(brew --prefix)/etc/bash_completion.d/{0}

You will need to start a new shell for this setup to take effect.
)",
                            rootName),
        .ValidArgsFunction = NoFileCompletions,
        .Args = NoArgs,
        .RunE = [out, noDesc](Command& cmd, std::span<const string>) { return cmd.Root()->GenBashCompletionV2(*out, !*noDesc); },
        .DisableFlagsInUseLine = true,
    });
    if (haveNoDescFlag) bash->Flags()->BoolVar(noDesc.get(), compCmdNoDescFlagName, compCmdNoDescFlagDefault, compCmdNoDescFlagDesc);

    auto zsh = New({
        .Use = "zsh"_s,
        .Short = fmt::Sprint("Generate the autocompletion script for {}", "zsh"),
        .Long = fmt::Sprint(R"(Generate the autocompletion script for the zsh shell.

If shell completion is not already enabled in your environment you will need
to enable it.  You can execute the following once:

	echo "autoload -U compinit; compinit" >> ~/.zshrc

To load completions in your current shell session:

	source <({0} completion zsh)

To load completions for every new session, execute once:

#### Linux:

	{0} completion zsh > "${{fpath[1]}}/_{0}"

#### macOS:

	{0} completion zsh > $(brew --prefix)/share/zsh/site-functions/_{0}

You will need to start a new shell for this setup to take effect.
)",
                            rootName),
        .ValidArgsFunction = NoFileCompletions,
        .Args = NoArgs,
        .RunE =
            [out, noDesc](Command& cmd, std::span<const string>) {
                if (*noDesc) return cmd.Root()->GenZshCompletionNoDesc(*out);
                return cmd.Root()->GenZshCompletion(*out);
            },
    });
    if (haveNoDescFlag) zsh->Flags()->BoolVar(noDesc.get(), compCmdNoDescFlagName, compCmdNoDescFlagDefault, compCmdNoDescFlagDesc);

    auto fish = New({
        .Use = "fish"_s,
        .Short = fmt::Sprint("Generate the autocompletion script for {}", "fish"),
        .Long = fmt::Sprint(R"(Generate the autocompletion script for the fish shell.

To load completions in your current shell session:

	{0} completion fish | source

To load completions for every new session, execute once:

	{0} completion fish > ~/.config/fish/completions/{0}.fish

You will need to start a new shell for this setup to take effect.
)",
                            rootName),
        .ValidArgsFunction = NoFileCompletions,
        .Args = NoArgs,
        .RunE = [out, noDesc](Command& cmd, std::span<const string>) { return cmd.Root()->GenFishCompletion(*out, !*noDesc); },
    });
    if (haveNoDescFlag) fish->Flags()->BoolVar(noDesc.get(), compCmdNoDescFlagName, compCmdNoDescFlagDefault, compCmdNoDescFlagDesc);

    auto powershell = New({
        .Use = "powershell"_s,
        .Short = fmt::Sprint("Generate the autocompletion script for {}", "powershell"),
        .Long = fmt::Sprint(R"(Generate the autocompletion script for powershell.

To load completions in your current shell session:

	{0} completion powershell | Out-String | Invoke-Expression

To load completions for every new session, add the output of the above command
to your powershell profile.
)",
                            rootName),
        .ValidArgsFunction = NoFileCompletions,
        .Args = NoArgs,
        .RunE =
            [out, noDesc](Command& cmd, std::span<const string>) {
                if (*noDesc) return cmd.Root()->GenPowerShellCompletion(*out);
                return cmd.Root()->GenPowerShellCompletionWithDesc(*out);
            },
    });
    if (haveNoDescFlag) {
        powershell->Flags()->BoolVar(noDesc.get(), compCmdNoDescFlagName, compCmdNoDescFlagDefault, compCmdNoDescFlagDesc);
    }

    completionCmd->AddCommand(bash, zsh, fish, powershell);
}

void CompDebug(std::string_view msg0, bool printToStdErr) {
    std::string msg = std::format("[Debug] {}", msg0);
    // Such logs are only printed when the user has set the environment
    // variable BASH_COMP_DEBUG_FILE to the path of some file to be used.
    string path = os::Getenv("BASH_COMP_DEBUG_FILE");
    if (!path.empty()) {
        auto [f, err] = os::OpenFile(path.view(), os::O_APPEND_ | os::O_CREATE_ | os::O_WRONLY_, 0644);
        if (!err) {
            WriteStringAndCheck(*f, msg);
            f->Close();
        }
    }
    if (printToStdErr) {
        // Must print to stderr for this not to be read by the completion
        // script.
        io::WriteString(os::Stderr, msg);
    }
}

void CompDebugln(std::string_view msg, bool printToStdErr) { CompDebug(std::format("{}\n", msg), printToStdErr); }

void CompError(std::string_view msg) { CompDebug(std::format("[Error] {}", msg), true); }

void CompErrorln(std::string_view msg) { CompError(std::format("{}\n", msg)); }

std::vector<Completion> AppendActiveHelp(std::vector<Completion> compArray, std::string_view activeHelpStr) {
    compArray.push_back(string(std::string(detail::activeHelpMarker) + std::string(activeHelpStr)));
    return compArray;
}

string GetActiveHelpConfig(const Command& cmd) {
    // If empty rootName, use the global env var.
    string activeHelpCfg = os::Getenv(activeHelpGlobalEnvVar);
    if (activeHelpCfg != activeHelpGlobalDisable) activeHelpCfg = os::Getenv(detail::activeHelpEnvVar(cmd.Root()->Name().view()).view());
    return activeHelpCfg;
}

// ---------------------------------------------------------------------------
// shell_completions.go

error MarkFlagRequired(pflag::FlagSet* flags, std::string_view name) {
    return flags->SetAnnotation(name, BashCompOneRequiredFlag, {"true"_s});
}

error MarkFlagFilename(pflag::FlagSet* flags, std::string_view name, std::vector<string> extensions) {
    return flags->SetAnnotation(name, BashCompFilenameExt, std::move(extensions));
}

error MarkFlagCustom(pflag::FlagSet* flags, std::string_view name, std::string_view f) {
    return flags->SetAnnotation(name, BashCompCustom, {string(f)});
}

error MarkFlagDirname(pflag::FlagSet* flags, std::string_view name) { return flags->SetAnnotation(name, BashCompSubdirsInDir, {}); }

error Command::MarkFlagRequired(std::string_view name) { return cmd::MarkFlagRequired(Flags(), name); }
error Command::MarkPersistentFlagRequired(std::string_view name) { return cmd::MarkFlagRequired(PersistentFlags(), name); }
error Command::markFlagFilename(pflag::FlagSet* flags, std::string_view name, std::vector<string> extensions) {
    return cmd::MarkFlagFilename(flags, name, std::move(extensions));
}
error Command::MarkFlagCustom(std::string_view name, std::string_view f) { return cmd::MarkFlagCustom(Flags(), name, f); }
error Command::MarkFlagDirname(std::string_view name) { return cmd::MarkFlagDirname(Flags(), name); }
error Command::MarkPersistentFlagDirname(std::string_view name) { return cmd::MarkFlagDirname(PersistentFlags(), name); }
error Command::MarkZshCompPositionalArgumentFile(int, std::vector<string>) { return nullptr; }
error Command::MarkZshCompPositionalArgumentWords(int, std::vector<string>) { return nullptr; }

}  // namespace gopp::cmd
