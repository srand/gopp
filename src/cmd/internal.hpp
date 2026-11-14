// The internals of the cmd package, shared by its files.
#pragma once

#include <gopp/bytes.hpp>
#include <gopp/cmd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace gopp::cmd::detail {

// A usage, help or version template, and the function that executes it.
struct tmplFunc {
    string tmpl;
    std::function<error(io::Writer&, const Command&)> fn;
};

struct commandState {
    // Groups for subcommands.
    std::vector<Group> commandgroups;
    // The arguments to parse instead of os::Args().
    std::optional<std::vector<string>> args;
    // Output of flag parsing (pflag prints deprecation notices there).
    ptr<bytes::Buffer> flagErrorBuf;
    // All flags; the persistent ones; the local ones; the inherited ones;
    // and the persistent flags of all parents.
    ptr<pflag::FlagSet> flags, pflags, lflags, iflags, parentsPflags;
    pflag::FlagSet::NormalizeFunc globNormFunc;

    std::function<error(Command&)> usageFunc;
    std::optional<tmplFunc> usageTemplate;
    std::function<error(Command&, error)> flagErrorFunc;
    std::optional<tmplFunc> helpTemplate;
    std::function<void(Command&, std::span<const string>)> helpFunc;
    ptr<Command> helpCommand;
    string helpCommandGroupID;
    string completionCommandGroupID;
    std::optional<tmplFunc> versionTemplate;
    string errPrefix;

    ptr<io::Reader> inReader;
    ptr<io::Writer> outWriter;
    ptr<io::Writer> errWriter;

    bool commandsAreSorted = false;
    // The name the command was called by, and whether it ran.
    string calledAsName;
    bool calledAsCalled = false;

    ptr<context::Context> ctx;

    std::vector<ptr<Command>> commands;
    Command* parent = nullptr;
    // Max lengths of the subcommands' Use, CommandPath and Name.
    std::size_t commandsMaxUseLen = 0;
    std::size_t commandsMaxCommandPathLen = 0;
    std::size_t commandsMaxNameLen = 0;

    // Commands removed while they run (__complete), kept alive as Go's
    // garbage collector would.
    std::vector<ptr<Command>> retired;

    // The flags this command registered completion functions for.
    std::vector<ptr<pflag::Flag>> completionFlags;

    ~commandState();
};

struct commandAccess {
    static commandState& s(const Command& c) { return *c.state_.s_; }
};

inline commandState& S(const Command& c) { return commandAccess::s(c); }

// Commands are never really const: a const method that changes state casts.
inline Command& mut(const Command& c) { return const_cast<Command&>(c); }

void mergePersistentFlags(const Command& c);
void updateParentsPflags(const Command& c);
std::vector<string> stripFlags(std::span<const string> args, const Command& c);
bool isFlagArg(std::string_view arg);
string findSuggestions(Command& c, std::string_view arg);
error legacyArgs(Command& cmd, std::span<const string> args);
bool stringInSlice(std::string_view a, std::span<const string> list);
std::size_t ld(std::string_view s, std::string_view t, bool ignoreCase);
string trimRightSpace(std::string_view s);
string rpad(std::string_view s, int padding);

// templates.cpp
tmplFunc tmpl(string text);
error defaultUsageFunc(io::Writer& w, const Command& c);
error defaultHelpFunc(io::Writer& w, const Command& c);
error defaultVersionFunc(io::Writer& w, const Command& c);

// completions.cpp
void initCompleteCmd(Command& c, std::span<const string> args);
void enforceFlagGroupsForCompletion(Command& c);
void forgetFlagCompletions(commandState& s);
string configEnvVar(std::string_view name, std::string_view suffix);
string getEnvConfig(const Command& cmd, std::string_view suffix);
inline constexpr std::string_view activeHelpMarker = "_activeHelp_ ";
string activeHelpEnvVar(std::string_view name);

// scripts.cpp
void genBashComp(std::string& buf, std::string_view name, bool includeDesc);
void genZshComp(std::string& buf, std::string_view name, bool includeDesc);
void genFishComp(std::string& buf, std::string_view name, bool includeDesc);
void genPowerShellComp(std::string& buf, std::string_view name, bool includeDesc);

}  // namespace gopp::cmd::detail
