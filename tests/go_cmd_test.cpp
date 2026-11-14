// Ported from spf13/cobra v1.10.2's command_test.go, args_test.go,
// flag_groups_test.go, completions_test.go and active_help_test.go, with
// cobra's test names. Tests of package internals use src/cmd/internal.hpp.
#include <gopp/bytes.hpp>
#include <gopp/cmd.hpp>
#include <gopp/context.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include "cmd/internal.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace gopp;
using cmd::Command;

namespace {

void emptyRun(Command&, std::span<const string>) {}

std::tuple<Command*, std::string, error> executeCommandC(Command& root, std::vector<string> args) {
    auto buf = make_ptr<bytes::Buffer>();
    root.SetOut(buf);
    root.SetErr(buf);
    root.SetArgs(std::move(args));
    auto [c, err] = root.ExecuteC();
    return {c, buf->String().str(), err};
}

std::pair<std::string, error> executeCommand(Command& root, std::vector<string> args = {}) {
    auto [_, output, err] = executeCommandC(root, std::move(args));
    return {output, err};
}

std::pair<std::string, error> executeCommandWithContext(ptr<context::Context> ctx, Command& root, std::vector<string> args) {
    auto buf = make_ptr<bytes::Buffer>();
    root.SetOut(buf);
    root.SetErr(buf);
    root.SetArgs(std::move(args));
    auto err = root.ExecuteContext(ctx);
    return {buf->String().str(), err};
}

std::string join(std::span<const string> v, std::string_view sep = " ") { return strings::Join(v, sep).str(); }

const std::string onetwo = "one two";

// The lines of a completion's output.
std::string lines(std::initializer_list<std::string_view> l) {
    std::string out;
    for (auto s : l) {
        out += s;
        out += "\n";
    }
    return out;
}

#define checkStringContains(got, expected) EXPECT_NE(std::string(got).find(expected), std::string::npos) << "Expected to contain:\n " << (expected) << "\nGot:\n " << (got)
#define checkStringOmits(got, expected) EXPECT_EQ(std::string(got).find(expected), std::string::npos) << "Expected to not contain:\n " << (expected) << "\nGot:\n " << (got)

// Restores a setting at the end of a test.
template <typename T>
struct restore {
    T& ref;
    T saved;
    explicit restore(T& r) : ref(r), saved(r) {}
    ~restore() { ref = saved; }
};

}  // namespace

// ---------------------------------------------------------------------------
// command_test.go

TEST(GoCmd, SingleCommand) {
    std::vector<string> rootCmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::ExactArgs(2),
                             .Run = [&](Command&, std::span<const string> args) { rootCmdArgs.assign(args.begin(), args.end()); }});
    auto aCmd = cmd::New({.Use = "a", .Args = cmd::NoArgs, .Run = emptyRun});
    auto bCmd = cmd::New({.Use = "b", .Args = cmd::NoArgs, .Run = emptyRun});
    rootCmd->AddCommand(aCmd, bCmd);

    auto [output, err] = executeCommand(*rootCmd, {"one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(rootCmdArgs), onetwo);
}

TEST(GoCmd, ChildCommand) {
    std::vector<string> child1CmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto child1Cmd = cmd::New({.Use = "child1", .Args = cmd::ExactArgs(2),
                               .Run = [&](Command&, std::span<const string> args) { child1CmdArgs.assign(args.begin(), args.end()); }});
    auto child2Cmd = cmd::New({.Use = "child2", .Args = cmd::NoArgs, .Run = emptyRun});
    rootCmd->AddCommand(child1Cmd, child2Cmd);

    auto [output, err] = executeCommand(*rootCmd, {"child1", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(child1CmdArgs), onetwo);
}

TEST(GoCmd, CallCommandWithoutSubcommands) {
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto [_, err] = executeCommand(*rootCmd);
    EXPECT_FALSE(err) << "Calling command without subcommands should not have error: " << err;
}

TEST(GoCmd, RootExecuteUnknownCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [output, _] = executeCommand(*rootCmd, {"unknown"});
    EXPECT_EQ(output, "Error: unknown command \"unknown\" for \"root\"\nRun 'root --help' for usage.\n");
}

TEST(GoCmd, SubcommandExecuteC) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    auto [c, output, err] = executeCommandC(*rootCmd, {"child"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(c->Name(), "child");
}

TEST(GoCmd, ExecuteContext) {
    auto ctx = context::TODO();
    auto ctxRun = [ctx](Command& cmd, std::span<const string>) { EXPECT_EQ(cmd.Context(), ctx) << "Command didn't get context"; };
    auto rootCmd = cmd::New({.Use = "root", .Run = ctxRun});
    auto childCmd = cmd::New({.Use = "child", .Run = ctxRun});
    auto granchildCmd = cmd::New({.Use = "grandchild", .Run = ctxRun});
    childCmd->AddCommand(granchildCmd);
    rootCmd->AddCommand(childCmd);
    for (auto args : std::vector<std::vector<string>>{{}, {"child"}, {"child", "grandchild"}}) {
        auto [_, err] = executeCommandWithContext(ctx, *rootCmd, args);
        EXPECT_FALSE(err) << err;
    }
}

TEST(GoCmd, Execute_NoContext) {
    auto run = [](Command& cmd, std::span<const string>) {
        EXPECT_EQ(cmd.Context(), context::Background()) << "Command should have the background context";
    };
    auto rootCmd = cmd::New({.Use = "root", .Run = run});
    auto childCmd = cmd::New({.Use = "child", .Run = run});
    auto granchildCmd = cmd::New({.Use = "grandchild", .Run = run});
    childCmd->AddCommand(granchildCmd);
    rootCmd->AddCommand(childCmd);
    for (auto args : std::vector<std::vector<string>>{{}, {"child"}, {"child", "grandchild"}}) {
        auto [_, err] = executeCommand(*rootCmd, args);
        EXPECT_FALSE(err) << err;
    }
}

TEST(GoCmd, RootUnknownCommandSilenced) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->SilenceErrors = true;
    rootCmd->SilenceUsage = true;
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [output, _] = executeCommand(*rootCmd, {"unknown"});
    EXPECT_EQ(output, "") << "Expected blank output, because of silenced usage.";
}

TEST(GoCmd, CommandAlias) {
    std::vector<string> timesCmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto echoCmd = cmd::New({.Use = "echo", .Aliases = {"say", "tell"}, .Args = cmd::NoArgs, .Run = emptyRun});
    auto timesCmd = cmd::New({.Use = "times", .Args = cmd::ExactArgs(2),
                              .Run = [&](Command&, std::span<const string> args) { timesCmdArgs.assign(args.begin(), args.end()); }});
    echoCmd->AddCommand(timesCmd);
    rootCmd->AddCommand(echoCmd);

    auto [output, err] = executeCommand(*rootCmd, {"tell", "times", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(timesCmdArgs), onetwo);
}

TEST(GoCmd, EnablePrefixMatching) {
    restore r(cmd::EnablePrefixMatching);
    cmd::EnablePrefixMatching = true;
    std::vector<string> aCmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto aCmd = cmd::New({.Use = "aCmd", .Args = cmd::ExactArgs(2),
                          .Run = [&](Command&, std::span<const string> args) { aCmdArgs.assign(args.begin(), args.end()); }});
    auto bCmd = cmd::New({.Use = "bCmd", .Args = cmd::NoArgs, .Run = emptyRun});
    rootCmd->AddCommand(aCmd, bCmd);

    auto [output, err] = executeCommand(*rootCmd, {"a", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(aCmdArgs), onetwo);
}

TEST(GoCmd, Plugin) {
    auto c = cmd::New({.Use = "kubectl-plugin",
                       .Args = cmd::NoArgs,
                       .Annotations = {{string::fromStatic(cmd::CommandDisplayNameAnnotation), "kubectl plugin"}},
                       .Version = "1.0.0",
                       .Run = emptyRun});
    auto [cmdHelp, err] = executeCommand(*c, {"-h"});
    EXPECT_FALSE(err) << err;
    checkStringContains(cmdHelp, "kubectl plugin [flags]");
    checkStringContains(cmdHelp, "help for kubectl plugin");
    checkStringContains(cmdHelp, "version for kubectl plugin");
}

TEST(GoCmd, ChildSameName) {
    std::vector<string> fooCmdArgs;
    auto rootCmd = cmd::New({.Use = "foo", .Args = cmd::NoArgs, .Run = emptyRun});
    auto fooCmd = cmd::New({.Use = "foo", .Args = cmd::ExactArgs(2),
                            .Run = [&](Command&, std::span<const string> args) { fooCmdArgs.assign(args.begin(), args.end()); }});
    auto barCmd = cmd::New({.Use = "bar", .Args = cmd::NoArgs, .Run = emptyRun});
    rootCmd->AddCommand(fooCmd, barCmd);

    auto [output, err] = executeCommand(*rootCmd, {"foo", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(fooCmdArgs), onetwo);
}

TEST(GoCmd, FlagLong) {
    std::vector<string> cArgs;
    auto c = cmd::New({.Use = "c", .Args = cmd::ArbitraryArgs,
                       .Run = [&](Command&, std::span<const string> args) { cArgs.assign(args.begin(), args.end()); }});
    isize intFlagValue = 0;
    string stringFlagValue;
    c->Flags()->IntVar(&intFlagValue, "intf", -1, "");
    c->Flags()->StringVar(&stringFlagValue, "sf", "", "");

    auto [output, err] = executeCommand(*c, {"--intf=7", "--sf=abc", "one", "--", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(c->ArgsLenAtDash(), 1);
    EXPECT_EQ(intFlagValue, 7);
    EXPECT_EQ(stringFlagValue, "abc");
    EXPECT_EQ(join(cArgs), onetwo);
}

TEST(GoCmd, FlagShort) {
    std::vector<string> cArgs;
    auto c = cmd::New({.Use = "c", .Args = cmd::ArbitraryArgs,
                       .Run = [&](Command&, std::span<const string> args) { cArgs.assign(args.begin(), args.end()); }});
    isize intFlagValue = 0;
    string stringFlagValue;
    c->Flags()->IntVarP(&intFlagValue, "intf", "i", -1, "");
    c->Flags()->StringVarP(&stringFlagValue, "sf", "s", "", "");

    auto [output, err] = executeCommand(*c, {"-i", "7", "-sabc", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(intFlagValue, 7);
    EXPECT_EQ(stringFlagValue, "abc");
    EXPECT_EQ(join(cArgs), onetwo);
}

TEST(GoCmd, ChildFlag) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    isize intFlagValue = 0;
    childCmd->Flags()->IntVarP(&intFlagValue, "intf", "i", -1, "");

    auto [output, err] = executeCommand(*rootCmd, {"child", "-i7"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(intFlagValue, 7);
}

TEST(GoCmd, ChildFlagWithParentLocalFlag) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    isize intFlagValue = 0;
    rootCmd->Flags()->StringP("sf", "s", "", "");
    childCmd->Flags()->IntVarP(&intFlagValue, "intf", "i", -1, "");

    auto [_, err] = executeCommand(*rootCmd, {"child", "-i7", "-sabc"});
    ASSERT_TRUE(err) << "Invalid flag should generate error";
    checkStringContains(err.Error().str(), "unknown shorthand");
    EXPECT_EQ(intFlagValue, 7);
}

TEST(GoCmd, FlagInvalidInput) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->Flags()->IntP("intf", "i", -1, "");
    auto [_, err] = executeCommand(*rootCmd, {"-iabc"});
    ASSERT_TRUE(err) << "Invalid flag value should generate error";
    checkStringContains(err.Error().str(), "invalid syntax");
}

TEST(GoCmd, FlagBeforeCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    isize flagValue = 0;
    childCmd->Flags()->IntVarP(&flagValue, "intf", "i", -1, "");

    // With short flag.
    auto [_, err] = executeCommand(*rootCmd, {"-i7", "child"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(flagValue, 7);

    // With long flag.
    std::tie(_, err) = executeCommand(*rootCmd, {"--intf=8", "child"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(flagValue, 8);
}

TEST(GoCmd, StripFlags) {
    struct {
        std::vector<string> input;
        std::vector<string> output;
    } tests[] = {
        {{"foo", "bar"}, {"foo", "bar"}},
        {{"foo", "--str", "-s"}, {"foo"}},
        {{"-s", "foo", "--str", "bar"}, {}},
        {{"-i10", "echo"}, {"echo"}},
        {{"-i=10", "echo"}, {"echo"}},
        {{"--int=100", "echo"}, {"echo"}},
        {{"-ib", "echo", "-sfoo", "baz"}, {"echo", "baz"}},
        {{"-i=baz", "bar", "-i", "foo", "blah"}, {"bar", "blah"}},
        {{"--int=baz", "-sbar", "-i", "foo", "blah"}, {"blah"}},
        {{"--bool", "bar", "-i", "foo", "blah"}, {"bar", "blah"}},
        {{"-b", "bar", "-i", "foo", "blah"}, {"bar", "blah"}},
        {{"--persist", "bar"}, {"bar"}},
        {{"-p", "bar"}, {"bar"}},
    };
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->PersistentFlags()->BoolP("persist", "p", false, "");
    c->Flags()->IntP("int", "i", -1, "");
    c->Flags()->StringP("str", "s", "", "");
    c->Flags()->BoolP("bool", "b", false, "");
    int i = 0;
    for (auto& test : tests) {
        auto got = cmd::detail::stripFlags(test.input, *c);
        EXPECT_EQ(test.output, got) << "(" << i << ")";
        i++;
    }
}

TEST(GoCmd, DisableFlagParsing) {
    std::vector<string> cArgs;
    auto c = cmd::New({.Use = "c",
                       .Run = [&](Command&, std::span<const string> args) { cArgs.assign(args.begin(), args.end()); },
                       .DisableFlagParsing = true});
    std::vector<string> args = {"cmd", "-v", "-race", "-file", "foo.go"};
    auto [output, err] = executeCommand(*c, args);
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(args, cArgs);
}

TEST(GoCmd, PersistentFlagsOnSameCommand) {
    std::vector<string> rootCmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::ArbitraryArgs,
                             .Run = [&](Command&, std::span<const string> args) { rootCmdArgs.assign(args.begin(), args.end()); }});
    isize flagValue = 0;
    rootCmd->PersistentFlags()->IntVarP(&flagValue, "intf", "i", -1, "");

    auto [output, err] = executeCommand(*rootCmd, {"-i7", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(rootCmdArgs), onetwo);
    EXPECT_EQ(flagValue, 7);
}

TEST(GoCmd, EmptyInputs) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    isize flagValue = 0;
    c->Flags()->IntVarP(&flagValue, "intf", "i", -1, "");
    auto [output, err] = executeCommand(*c, {"", "-i7", ""});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(flagValue, 7);
}

TEST(GoCmd, ChildFlagShadowsParentPersistentFlag) {
    auto parent = cmd::New({.Use = "parent", .Run = emptyRun});
    auto child = cmd::New({.Use = "child", .Run = emptyRun});
    parent->PersistentFlags()->Bool("boolf", false, "");
    parent->PersistentFlags()->Int("intf", -1, "");
    child->Flags()->String("strf", "", "");
    child->Flags()->Int("intf", -1, "");
    parent->AddCommand(child);

    auto* childInherited = child->InheritedFlags();
    auto* childLocal = child->LocalFlags();
    EXPECT_TRUE(childLocal->Lookup("strf")) << "LocalFlags expected to contain \"strf\"";
    EXPECT_TRUE(childInherited->Lookup("boolf")) << "InheritedFlags expected to contain \"boolf\"";
    EXPECT_FALSE(childInherited->Lookup("intf")) << "InheritedFlags should not contain shadowed flag \"intf\"";
    EXPECT_TRUE(childLocal->Lookup("intf")) << "LocalFlags expected to contain \"intf\"";
}

TEST(GoCmd, PersistentFlagsOnChild) {
    std::vector<string> childCmdArgs;
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Args = cmd::ArbitraryArgs,
                              .Run = [&](Command&, std::span<const string> args) { childCmdArgs.assign(args.begin(), args.end()); }});
    rootCmd->AddCommand(childCmd);
    isize parentFlagValue = 0, childFlagValue = 0;
    rootCmd->PersistentFlags()->IntVarP(&parentFlagValue, "parentf", "p", -1, "");
    childCmd->Flags()->IntVarP(&childFlagValue, "childf", "c", -1, "");

    auto [output, err] = executeCommand(*rootCmd, {"child", "-c7", "-p8", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(join(childCmdArgs), onetwo);
    EXPECT_EQ(parentFlagValue, 8);
    EXPECT_EQ(childFlagValue, 7);
}

TEST(GoCmd, RequiredFlags) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->Flags()->String("foo1", "", "");
    EXPECT_FALSE(c->MarkFlagRequired("foo1"));
    c->Flags()->String("foo2", "", "");
    EXPECT_FALSE(c->MarkFlagRequired("foo2"));
    c->Flags()->String("bar", "", "");

    auto [_, err] = executeCommand(*c);
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "required flag(s) \"foo1\", \"foo2\" not set");
}

TEST(GoCmd, PersistentRequiredFlags) {
    auto parent = cmd::New({.Use = "parent", .Run = emptyRun});
    parent->PersistentFlags()->String("foo1", "", "");
    EXPECT_FALSE(parent->MarkPersistentFlagRequired("foo1"));
    parent->PersistentFlags()->String("foo2", "", "");
    EXPECT_FALSE(parent->MarkPersistentFlagRequired("foo2"));
    parent->Flags()->String("foo3", "", "");

    auto child = cmd::New({.Use = "child", .Run = emptyRun});
    child->Flags()->String("bar1", "", "");
    EXPECT_FALSE(child->MarkFlagRequired("bar1"));
    child->Flags()->String("bar2", "", "");
    EXPECT_FALSE(child->MarkFlagRequired("bar2"));
    child->Flags()->String("bar3", "", "");
    parent->AddCommand(child);

    auto [_, err] = executeCommand(*parent, {"child"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "required flag(s) \"bar1\", \"bar2\", \"foo1\", \"foo2\" not set");
}

TEST(GoCmd, PersistentRequiredFlagsWithDisableFlagParsing) {
    // Make sure a required persistent flag does not break commands that
    // disable flag parsing.
    auto parent = cmd::New({.Use = "parent", .Run = emptyRun});
    parent->PersistentFlags()->Bool("foo", false, "");
    auto* flag = parent->PersistentFlags()->Lookup("foo");
    EXPECT_FALSE(parent->MarkPersistentFlagRequired("foo"));

    auto child = cmd::New({.Use = "child", .Run = emptyRun});
    child->DisableFlagParsing = true;
    parent->AddCommand(child);

    auto [_, err] = executeCommand(*parent, {"--foo", "child"});
    EXPECT_FALSE(err) << err;
    // Reset the flag or else it will remember the state from the previous
    // command.
    flag->Changed = false;
    std::tie(_, err) = executeCommand(*parent, {"child", "--foo"});
    EXPECT_FALSE(err) << err;
    flag->Changed = false;
    std::tie(_, err) = executeCommand(*parent, {"child"});
    EXPECT_FALSE(err) << err;
}

TEST(GoCmd, InitHelpFlagMergesFlags) {
    std::string usage = "custom flag";
    auto rootCmd = cmd::New({.Use = "root"});
    rootCmd->PersistentFlags()->Bool("help", false, "custom flag");
    auto childCmd = cmd::New({.Use = "child"});
    rootCmd->AddCommand(childCmd);
    childCmd->InitDefaultHelpFlag();
    auto* got = childCmd->Flags()->Lookup("help");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->Usage, usage);
}

TEST(GoCmd, HelpCommandExecuted) {
    auto rootCmd = cmd::New({.Use = "root", .Long = "Long description", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd, {"help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, rootCmd->Long.str());
}

TEST(GoCmd, HelpCommandExecutedOnChild) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Long = "Long description", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    auto [output, err] = executeCommand(*rootCmd, {"help", "child"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, childCmd->Long.str());
}

TEST(GoCmd, HelpCommandExecutedOnChildWithFlagThatShadowsParentFlag) {
    auto parent = cmd::New({.Use = "parent", .Run = emptyRun});
    auto child = cmd::New({.Use = "child", .Run = emptyRun});
    parent->AddCommand(child);
    parent->PersistentFlags()->Bool("foo", false, "parent foo usage");
    parent->PersistentFlags()->Bool("bar", false, "parent bar usage");
    child->Flags()->Bool("foo", false, "child foo usage");  // This shadows parent's foo flag
    child->Flags()->Bool("baz", false, "child baz usage");

    auto [got, err] = executeCommand(*parent, {"help", "child"});
    EXPECT_FALSE(err) << err;
    std::string expected = R"(Usage:
  parent child [flags]

Flags:
      --baz    child baz usage
      --foo    child foo usage
  -h, --help   help for child

Global Flags:
      --bar   parent bar usage
)";
    EXPECT_EQ(got, expected);
}

TEST(GoCmd, SetHelpCommand) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->AddCommand(cmd::New({.Use = "empty", .Run = emptyRun}));
    std::string expected = "WORKS";
    c->SetHelpCommand(cmd::New({
        .Use = "help [command]",
        .Short = "Help about any command",
        .Long = "Help provides help for any command in the application.\n\tSimply type "_s + c->Name() + " help [path to command] for full details.",
        .Run = [&](Command& c, std::span<const string>) { c.Print(expected); },
    }));
    auto [got, err] = executeCommand(*c, {"help"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(got, expected);
}

TEST(GoCmd, SetHelpTemplate) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->SetHelpTemplate("WORKS {{.UseLine}}");

    // Call the help on the root command and check the new template is used.
    auto [got, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(got, "WORKS " + rootCmd->UseLine().str());

    // Call the help on the child command and check the new template is
    // inherited from the parent.
    std::tie(got, err) = executeCommand(*rootCmd, {"child", "--help"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(got, "WORKS " + childCmd->UseLine().str());

    // Reset the root command help template and make sure it falls back to
    // the default.
    rootCmd->SetHelpTemplate("");
    std::tie(got, err) = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(got, "Usage:");
}

TEST(GoCmd, HelpFlagExecuted) {
    auto rootCmd = cmd::New({.Use = "root", .Long = "Long description", .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, rootCmd->Long.str());
}

TEST(GoCmd, HelpFlagExecutedOnChild) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Long = "Long description", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    auto [output, err] = executeCommand(*rootCmd, {"child", "--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, childCmd->Long.str());
}

TEST(GoCmd, HelpFlagInHelp) {
    auto parentCmd = cmd::New({.Use = "parent", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    parentCmd->AddCommand(childCmd);
    auto [output, err] = executeCommand(*parentCmd, {"help", "child"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "[flags]");
}

TEST(GoCmd, FlagsInUsage) {
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "[flags]");
}

TEST(GoCmd, HelpExecutedOnNonRunnableChild) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Long = "Long description"});
    rootCmd->AddCommand(childCmd);
    auto [output, err] = executeCommand(*rootCmd, {"child"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, childCmd->Long.str());
}

TEST(GoCmd, SetUsageTemplate) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->SetUsageTemplate("WORKS {{.UseLine}}");

    // Trigger the usage on the root command and check the new template is
    // used.
    auto [got, err] = executeCommand(*rootCmd, {"--invalid"});
    EXPECT_TRUE(err) << "Expected error but did not get one";
    checkStringContains(got, "WORKS " + rootCmd->UseLine().str());

    // Trigger the usage on the child command and check the new template is
    // inherited from the parent.
    std::tie(got, err) = executeCommand(*rootCmd, {"child", "--invalid"});
    EXPECT_TRUE(err) << "Expected error but did not get one";
    checkStringContains(got, "WORKS " + childCmd->UseLine().str());

    // Reset the root command usage template and make sure it falls back to
    // the default.
    rootCmd->SetUsageTemplate("");
    std::tie(got, err) = executeCommand(*rootCmd, {"--invalid"});
    EXPECT_TRUE(err) << "Expected error but did not get one";
    checkStringContains(got, "Usage:");
}

TEST(GoCmd, VersionFlagExecuted) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--version", "arg1"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "root version 1.0.0");
}

TEST(GoCmd, VersionFlagExecutedDiplayName) {
    auto rootCmd = cmd::New({.Use = "kubectl-plugin",
                             .Annotations = {{string::fromStatic(cmd::CommandDisplayNameAnnotation), "kubectl plugin"}},
                             .Version = "1.0.0",
                             .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--version", "arg1"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "kubectl plugin version 1.0.0");
}

TEST(GoCmd, VersionFlagExecutedWithNoName) {
    auto rootCmd = cmd::New({.Version = "1.0.0", .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--version", "arg1"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "version 1.0.0");
}

TEST(GoCmd, ShortAndLongVersionFlagInHelp) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "-v, --version");
}

TEST(GoCmd, LongVersionFlagOnlyInHelpWhenShortPredefined) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    rootCmd->Flags()->StringP("foo", "v", "", "not a version flag");
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringOmits(output, "-v, --version");
    checkStringContains(output, "--version");
}

TEST(GoCmd, ShorthandVersionFlagExecuted) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    auto [output, err] = executeCommand(*rootCmd, {"-v", "arg1"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "root version 1.0.0");
}

TEST(GoCmd, VersionTemplate) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    rootCmd->SetVersionTemplate("customized version: {{.Version}}");
    auto [output, err] = executeCommand(*rootCmd, {"--version", "arg1"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "customized version: 1.0.0");
}

TEST(GoCmd, RootErrPrefixExecutedOnSubcommand) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->SetErrPrefix("root error prefix:");
    rootCmd->AddCommand(cmd::New({.Use = "sub", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd, {"sub", "--unknown-flag"});
    EXPECT_TRUE(err) << "Expected error";
    checkStringContains(output, "root error prefix: unknown flag: --unknown-flag");
}

TEST(GoCmd, RootAndSubErrPrefix) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto subCmd = cmd::New({.Use = "sub", .Run = emptyRun});
    rootCmd->AddCommand(subCmd);
    rootCmd->SetErrPrefix("root error prefix:");
    subCmd->SetErrPrefix("sub error prefix:");

    auto [output, err] = executeCommand(*rootCmd, {"--unknown-root-flag"});
    EXPECT_TRUE(err) << "Expected error";
    checkStringContains(output, "root error prefix: unknown flag: --unknown-root-flag");

    std::tie(output, err) = executeCommand(*rootCmd, {"sub", "--unknown-sub-flag"});
    EXPECT_TRUE(err) << "Expected error";
    checkStringContains(output, "sub error prefix: unknown flag: --unknown-sub-flag");
}

TEST(GoCmd, VersionFlagExecutedOnSubcommand) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0"});
    rootCmd->AddCommand(cmd::New({.Use = "sub", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd, {"--version", "sub"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "root version 1.0.0");
}

TEST(GoCmd, VersionFlagOnlyAddedToRoot) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.0.0", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "sub", .Run = emptyRun}));
    auto [_, err] = executeCommand(*rootCmd, {"sub", "--version"});
    ASSERT_TRUE(err) << "Expected error";
    checkStringContains(err.Error().str(), "unknown flag: --version");
}

TEST(GoCmd, VersionFlagOnlyExistsIfVersionNonEmpty) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto [_, err] = executeCommand(*rootCmd, {"--version"});
    ASSERT_TRUE(err) << "Expected error";
    checkStringContains(err.Error().str(), "unknown flag: --version");
}

TEST(GoCmd, ShorthandVersionFlagOnlyAddedIfShorthandNotDefined) {
    auto rootCmd = cmd::New({.Use = "root", .Version = "1.2.3", .Run = emptyRun});
    rootCmd->Flags()->StringP("notversion", "v", "", "not a version flag");
    auto [_, err] = executeCommand(*rootCmd, {"-v"});
    ASSERT_TRUE(err) << "Expected error";
    auto* got = rootCmd->Flags()->ShorthandLookup("v");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->Name, "notversion");
    checkStringContains(err.Error().str(), "flag needs an argument");
}

TEST(GoCmd, UsageIsNotPrintedTwice) {
    auto c = cmd::New({.Use = "root"});
    auto sub = cmd::New({.Use = "sub"});
    c->AddCommand(sub);
    auto [output, _] = executeCommand(*c, {""});
    EXPECT_EQ(strings::Count(output, "Usage:"), 1) << "Usage output is not printed exactly once";
}

TEST(GoCmd, VisitParents) {
    auto c = cmd::New({.Use = "app"});
    auto sub = cmd::New({.Use = "sub"});
    auto dsub = cmd::New({.Use = "dsub"});
    sub->AddCommand(dsub);
    c->AddCommand(sub);
    int total = 0;
    auto add = [&](Command&) { total++; };
    sub->VisitParents(add);
    EXPECT_EQ(total, 1);
    total = 0;
    dsub->VisitParents(add);
    EXPECT_EQ(total, 2);
    total = 0;
    c->VisitParents(add);
    EXPECT_EQ(total, 0);
}

TEST(GoCmd, Suggestions) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto timesCmd = cmd::New({.Use = "times", .SuggestFor = {"counts"}, .Run = emptyRun});
    rootCmd->AddCommand(timesCmd);

    std::map<std::string, std::string> tests = {
        {"time", "times"}, {"tiems", "times"}, {"tims", "times"},   {"timeS", "times"},  {"rimes", "times"}, {"ti", "times"},
        {"t", "times"},    {"timely", "times"}, {"ri", ""},         {"timezone", ""},    {"foo", ""},        {"counts", "times"},
    };
    for (auto& [typo, suggestion] : tests) {
        for (bool suggestionsDisabled : {true, false}) {
            rootCmd->DisableSuggestions = suggestionsDisabled;
            auto [output, _] = executeCommand(*rootCmd, {string(typo)});
            std::string expected;
            if (suggestion.empty() || suggestionsDisabled) {
                expected = std::format("Error: unknown command \"{}\" for \"root\"\nRun 'root --help' for usage.\n", typo);
            } else {
                expected = std::format("Error: unknown command \"{}\" for \"root\"\n\nDid you mean this?\n\t{}\n\nRun 'root --help' for usage.\n",
                                       typo, suggestion);
            }
            EXPECT_EQ(output, expected);
        }
    }
}

TEST(GoCmd, CaseInsensitive) {
    restore r(cmd::EnableCaseInsensitive);
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Aliases = {"alternative"}, .Run = emptyRun});
    auto granchildCmd = cmd::New({.Use = "GRANDCHILD", .Aliases = {"ALIAS"}, .Run = emptyRun});
    childCmd->AddCommand(granchildCmd);
    rootCmd->AddCommand(childCmd);

    struct {
        std::vector<string> args;
        bool failWithoutEnabling;
    } tests[] = {
        {{"child"}, false},
        {{"CHILD"}, true},
        {{"chILD"}, true},
        {{"CHIld"}, true},
        {{"alternative"}, false},
        {{"ALTERNATIVE"}, true},
        {{"ALTernatIVE"}, true},
        {{"alternatiVE"}, true},
        {{"child", "GRANDCHILD"}, false},
        {{"child", "grandchild"}, true},
        {{"CHIld", "GRANdchild"}, true},
        {{"alternative", "ALIAS"}, false},
        {{"alternative", "alias"}, true},
        {{"CHILD", "alias"}, true},
        {{"CHIld", "aliAS"}, true},
    };
    for (auto& test : tests) {
        for (bool enableCaseInsensitivity : {true, false}) {
            cmd::EnableCaseInsensitive = enableCaseInsensitivity;
            auto [output, err] = executeCommand(*rootCmd, test.args);
            bool expectedFailure = test.failWithoutEnabling && !enableCaseInsensitivity;
            if (!expectedFailure) {
                EXPECT_EQ(output, "") << join(test.args);
                EXPECT_FALSE(err) << join(test.args) << ": " << err;
            }
        }
    }
}

TEST(GoCmd, RemoveCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->RemoveCommand(childCmd);
    auto [_, err] = executeCommand(*rootCmd, {"child"});
    EXPECT_TRUE(err) << "Expected error on calling removed command. Got nil.";
}

TEST(GoCmd, ReplaceCommandWithRemove) {
    int childUsed = 0;
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto child1Cmd = cmd::New({.Use = "child", .Run = [&](Command&, std::span<const string>) { childUsed = 1; }});
    auto child2Cmd = cmd::New({.Use = "child", .Run = [&](Command&, std::span<const string>) { childUsed = 2; }});
    rootCmd->AddCommand(child1Cmd);
    rootCmd->RemoveCommand(child1Cmd);
    rootCmd->AddCommand(child2Cmd);
    auto [output, err] = executeCommand(*rootCmd, {"child"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(childUsed, 2);
}

TEST(GoCmd, DeprecatedCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto deprecatedCmd = cmd::New({.Use = "deprecated", .Deprecated = "This command is deprecated", .Run = emptyRun});
    rootCmd->AddCommand(deprecatedCmd);
    auto [output, err] = executeCommand(*rootCmd, {"deprecated"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, deprecatedCmd->Deprecated.str());
}

TEST(GoCmd, Hooks) {
    std::string persPreArgs, preArgs, runArgs, postArgs, persPostArgs;
    auto c = cmd::New({
        .Use = "c",
        .PersistentPreRun = [&](Command&, std::span<const string> args) { persPreArgs = join(args); },
        .PreRun = [&](Command&, std::span<const string> args) { preArgs = join(args); },
        .Run = [&](Command&, std::span<const string> args) { runArgs = join(args); },
        .PostRun = [&](Command&, std::span<const string> args) { postArgs = join(args); },
        .PersistentPostRun = [&](Command&, std::span<const string> args) { persPostArgs = join(args); },
    });
    auto [output, err] = executeCommand(*c, {"one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(persPreArgs, onetwo);
    EXPECT_EQ(preArgs, onetwo);
    EXPECT_EQ(runArgs, onetwo);
    EXPECT_EQ(postArgs, onetwo);
    EXPECT_EQ(persPostArgs, onetwo);
}

namespace {

void testPersistentHooks(const std::vector<std::string>& expectedHookRunOrder) {
    std::vector<std::string> hookRunOrder;
    auto hook = [&](std::string name) {
        return [&hookRunOrder, name](Command&, std::span<const string> args) {
            hookRunOrder.push_back(name);
            EXPECT_EQ(join(args), onetwo) << name;
        };
    };
    auto parentCmd = cmd::New({
        .Use = "parent",
        .PersistentPreRun = hook("parent PersistentPreRun"),
        .PreRun = hook("parent PreRun"),
        .Run = hook("parent Run"),
        .PostRun = hook("parent PostRun"),
        .PersistentPostRun = hook("parent PersistentPostRun"),
    });
    auto childCmd = cmd::New({
        .Use = "child",
        .PersistentPreRun = hook("child PersistentPreRun"),
        .PreRun = hook("child PreRun"),
        .Run = hook("child Run"),
        .PostRun = hook("child PostRun"),
        .PersistentPostRun = hook("child PersistentPostRun"),
    });
    parentCmd->AddCommand(childCmd);
    auto [output, err] = executeCommand(*parentCmd, {"child", "one", "two"});
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(hookRunOrder, expectedHookRunOrder);
}

}  // namespace

TEST(GoCmd, PersistentHooks) {
    restore r(cmd::EnableTraverseRunHooks);
    cmd::EnableTraverseRunHooks = true;
    testPersistentHooks({
        "parent PersistentPreRun",
        "child PersistentPreRun",
        "child PreRun",
        "child Run",
        "child PostRun",
        "child PersistentPostRun",
        "parent PersistentPostRun",
    });
    cmd::EnableTraverseRunHooks = false;
    testPersistentHooks({
        "child PersistentPreRun",
        "child PreRun",
        "child Run",
        "child PostRun",
        "child PersistentPostRun",
    });
}

TEST(GoCmd, NormPassedOnLocal) {
    auto toUpper = [](pflag::FlagSet*, std::string_view name) { return strings::ToUpper(name); };
    auto c = cmd::New({});
    c->Flags()->Bool("flagname", true, "this is a dummy flag");
    c->SetGlobalNormalizationFunc(toUpper);
    EXPECT_EQ(toUpper(nullptr, "flagname"), c->LocalFlags()->Lookup("flagname")->Name);
}

TEST(GoCmd, NormPassedOnInherited) {
    auto toUpper = [](pflag::FlagSet*, std::string_view name) { return strings::ToUpper(name); };
    auto c = cmd::New({});
    c->SetGlobalNormalizationFunc(toUpper);
    auto child1 = cmd::New({});
    c->AddCommand(child1);
    c->PersistentFlags()->Bool("flagname", true, "");
    auto child2 = cmd::New({});
    c->AddCommand(child2);
    auto* inherited = child1->InheritedFlags();
    ASSERT_TRUE(inherited->Lookup("flagname"));
    EXPECT_EQ(inherited->Lookup("flagname")->Name, toUpper(nullptr, "flagname"));
    inherited = child2->InheritedFlags();
    ASSERT_TRUE(inherited->Lookup("flagname"));
    EXPECT_EQ(inherited->Lookup("flagname")->Name, toUpper(nullptr, "flagname"));
}

TEST(GoCmd, ConsistentNormalizedName) {
    auto toUpper = [](pflag::FlagSet*, std::string_view name) { return strings::ToUpper(name); };
    auto n = [](pflag::FlagSet*, std::string_view name) { return string(name); };
    auto c = cmd::New({});
    c->Flags()->Bool("flagname", true, "");
    c->SetGlobalNormalizationFunc(toUpper);
    c->SetGlobalNormalizationFunc(n);
    EXPECT_FALSE(c->LocalFlags()->Lookup("flagname")) << "Normalizing flag names should not result in duplicate flags";
}

TEST(GoCmd, HiddenCommandExecutes) {
    // Ensure that a hidden command executes.
    bool executed = false;
    auto c = cmd::New({.Use = "c", .Run = [&](Command&, std::span<const string>) { executed = true; }, .Hidden = true});
    auto [output, err] = executeCommand(*c);
    EXPECT_EQ(output, "");
    EXPECT_FALSE(err) << err;
    EXPECT_TRUE(executed) << "Hidden command should have been executed";
}

TEST(GoCmd, HiddenCommandIsHidden) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun, .Hidden = true});
    EXPECT_FALSE(c->IsAvailableCommand()) << "Hidden command should be unavailable";
}

TEST(GoCmd, CommandsAreSorted) {
    restore r(cmd::EnableCommandSorting);
    cmd::EnableCommandSorting = true;
    auto rootCmd = cmd::New({.Use = "root"});
    for (auto name : {"middle", "zlast", "afirst"}) rootCmd->AddCommand(cmd::New({.Use = name}));
    std::vector<std::string> got;
    for (auto& c : rootCmd->Commands()) got.push_back(c->Name().str());
    EXPECT_EQ(got, (std::vector<std::string>{"afirst", "middle", "zlast"}));
}

TEST(GoCmd, EnableCommandSortingIsDisabled) {
    restore r(cmd::EnableCommandSorting);
    cmd::EnableCommandSorting = false;
    auto rootCmd = cmd::New({.Use = "root"});
    for (auto name : {"middle", "zlast", "afirst"}) rootCmd->AddCommand(cmd::New({.Use = name}));
    std::vector<std::string> got;
    for (auto& c : rootCmd->Commands()) got.push_back(c->Name().str());
    EXPECT_EQ(got, (std::vector<std::string>{"middle", "zlast", "afirst"}));
}

TEST(GoCmd, UsageWithGroup) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->CompletionOptions.DisableDefaultCmd = true;
    rootCmd->AddGroup(cmd::Group{.ID = "group1", .Title = "group1"});
    rootCmd->AddGroup(cmd::Group{.ID = "group2", .Title = "group2"});
    rootCmd->AddCommand(cmd::New({.Use = "cmd1", .GroupID = "group1", .Run = emptyRun}));
    rootCmd->AddCommand(cmd::New({.Use = "cmd2", .GroupID = "group2", .Run = emptyRun}));

    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    // help should be ungrouped here
    checkStringContains(output, "\nAdditional Commands:\n  help");
    checkStringContains(output, "\ngroup1\n  cmd1");
    checkStringContains(output, "\ngroup2\n  cmd2");
}

TEST(GoCmd, UsageHelpGroup) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->CompletionOptions.DisableDefaultCmd = true;
    rootCmd->AddGroup(cmd::Group{.ID = "group", .Title = "group"});
    rootCmd->AddCommand(cmd::New({.Use = "xxx", .GroupID = "group", .Run = emptyRun}));
    rootCmd->SetHelpCommandGroupID("group");

    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    // now help should be grouped under "group"
    checkStringOmits(output, "\nAdditional Commands:\n  help");
    checkStringContains(output, "\ngroup\n  help");
}

TEST(GoCmd, UsageCompletionGroup) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->AddGroup(cmd::Group{.ID = "group", .Title = "group"});
    rootCmd->AddGroup(cmd::Group{.ID = "help", .Title = "help"});
    rootCmd->AddCommand(cmd::New({.Use = "xxx", .GroupID = "group", .Run = emptyRun}));
    rootCmd->SetHelpCommandGroupID("help");
    rootCmd->SetCompletionCommandGroupID("group");

    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    // Now completion should be grouped under "group".
    checkStringOmits(output, "\nAdditional Commands:\n  completion");
    checkStringContains(output, "\ngroup\n  completion");
}

TEST(GoCmd, UngroupedCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->AddGroup(cmd::Group{.ID = "group", .Title = "group"});
    rootCmd->AddGroup(cmd::Group{.ID = "help", .Title = "help"});
    rootCmd->AddCommand(cmd::New({.Use = "xxx", .GroupID = "group", .Run = emptyRun}));
    rootCmd->SetHelpCommandGroupID("help");
    rootCmd->SetCompletionCommandGroupID("group");
    // Add a command without a group.
    rootCmd->AddCommand(cmd::New({.Use = "yyy", .Run = emptyRun}));

    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    // The yyy command should be in the additional command "group".
    checkStringContains(output, "\nAdditional Commands:\n  yyy");
}

TEST(GoCmd, AddGroup) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->AddGroup(cmd::Group{.ID = "group", .Title = "Test group"});
    rootCmd->AddCommand(cmd::New({.Use = "cmd", .GroupID = "group", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "\nTest group\n  cmd");
}

TEST(GoCmd, WrongGroupFirstLevel) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "test", .Run = emptyRun});
    rootCmd->AddGroup(cmd::Group{.ID = "group", .Title = "Test group"});
    // Use the wrong group ID.
    rootCmd->AddCommand(cmd::New({.Use = "cmd", .GroupID = "wrong", .Run = emptyRun}));
    EXPECT_DEATH(executeCommand(*rootCmd, {"--help"}), "group id 'wrong' is not defined for subcommand 'root cmd'");
}

TEST(GoCmd, SetOutput) {
    auto c = cmd::New({});
    c->SetOutput(nullptr);
    EXPECT_EQ(&c->OutOrStdout(), static_cast<io::Writer*>(&os::Stdout)) << "Expected setting output to nil to revert back to stdout";
}

TEST(GoCmd, SetOut) {
    auto c = cmd::New({});
    c->SetOut(nullptr);
    EXPECT_EQ(&c->OutOrStdout(), static_cast<io::Writer*>(&os::Stdout)) << "Expected setting output to nil to revert back to stdout";
}

TEST(GoCmd, SetErr) {
    auto c = cmd::New({});
    c->SetErr(nullptr);
    EXPECT_EQ(&c->ErrOrStderr(), static_cast<io::Writer*>(&os::Stderr)) << "Expected setting error to nil to revert back to stderr";
}

TEST(GoCmd, SetIn) {
    auto c = cmd::New({});
    c->SetIn(nullptr);
    EXPECT_EQ(&c->InOrStdin(), static_cast<io::Reader*>(&os::Stdin)) << "Expected setting input to nil to revert back to stdin";
}

TEST(GoCmd, UsageStringRedirected) {
    auto c = cmd::New({});
    c->SetUsageFunc([](Command& cmd) -> error {
        cmd.Print("[stdout1]");
        cmd.PrintErr("[stderr2]");
        cmd.Print("[stdout3]");
        return nullptr;
    });
    EXPECT_EQ(c->UsageString(), "[stdout1][stderr2][stdout3]") << "Expected usage string to consider both stdout and stderr";
}

TEST(GoCmd, CommandPrintRedirection) {
    auto errBuff = make_ptr<bytes::Buffer>();
    auto outBuff = make_ptr<bytes::Buffer>();
    auto root = cmd::New({.Run = [](Command& cmd, std::span<const string>) {
        cmd.PrintErr("PrintErr");
        cmd.PrintErrln("PrintErr{}", "line");
        cmd.PrintErrf("PrintEr{}", "r");
        cmd.Print("Print");
        cmd.Println("Print{}", "line");
        cmd.Printf("Prin{}", "t");
    }});
    root->SetErr(errBuff);
    root->SetOut(outBuff);
    root->SetArgs({});
    EXPECT_FALSE(root->Execute());
    EXPECT_EQ(errBuff->String(), "PrintErrPrintErrline\nPrintErr");
    EXPECT_EQ(outBuff->String(), "PrintPrintline\nPrint");
}

TEST(GoCmd, FlagErrorFunc) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->SetFlagErrorFunc([](Command&, error err) { return fmt::Errorf("This is expected: {}", err); });
    auto [_, err] = executeCommand(*c, {"--unknown-flag"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "This is expected: unknown flag: --unknown-flag");
}

TEST(GoCmd, FlagErrorFuncHelp) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->PersistentFlags()->Bool("help", false, "help for c");
    c->SetFlagErrorFunc([](Command&, error err) { return fmt::Errorf("wrap error: {}", fmt::W(err)); });

    auto [out, err] = executeCommand(*c, {"--help"});
    EXPECT_FALSE(err) << "--help should not fail: " << err;
    std::string expected = R"(Usage:
  c [flags]

Flags:
      --help   help for c
)";
    EXPECT_EQ(out, expected);

    std::tie(out, err) = executeCommand(*c, {"-h"});
    EXPECT_FALSE(err) << "-h should not fail: " << err;
    EXPECT_EQ(out, expected);
}

TEST(GoCmd, SortedFlags) {
    auto c = cmd::New({});
    c->Flags()->SortFlags = false;
    std::vector<string> names = {"C", "B", "A", "D"};
    for (auto& name : names) c->Flags()->Bool(name.view(), false, "");
    std::size_t i = 0;
    c->LocalFlags()->VisitAll([&](pflag::Flag* f) {
        if (i == names.size()) return;
        if (cmd::detail::stringInSlice(f->Name.view(), names)) {
            EXPECT_EQ(names[i], f->Name) << "Incorrect order";
            i++;
        }
    });
}

TEST(GoCmd, UseDeprecatedFlags) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->Flags()->BoolP("deprecated", "d", false, "deprecated flag");
    EXPECT_FALSE(c->Flags()->MarkDeprecated("deprecated", "This flag is deprecated"));
    auto [output, err] = executeCommand(*c, {"c", "-d"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, "This flag is deprecated");
}

TEST(GoCmd, TraverseWithParentFlags) {
    auto rootCmd = cmd::New({.Use = "root", .TraverseChildren = true});
    rootCmd->Flags()->String("str", "", "");
    rootCmd->Flags()->BoolP("bool", "b", false, "");
    auto childCmd = cmd::New({.Use = "child"});
    childCmd->Flags()->Int("int", -1, "");
    rootCmd->AddCommand(childCmd);

    std::vector<string> in = {"-b", "--str", "ok", "child", "--int"};
    auto [c, args, err] = rootCmd->Traverse(in);
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(args, std::vector<string>{"--int"});
    EXPECT_EQ(c->Name(), childCmd->Name());
}

TEST(GoCmd, TraverseNoParentFlags) {
    auto rootCmd = cmd::New({.Use = "root", .TraverseChildren = true});
    rootCmd->Flags()->String("foo", "", "foo things");
    auto childCmd = cmd::New({.Use = "child"});
    childCmd->Flags()->String("str", "", "");
    rootCmd->AddCommand(childCmd);

    std::vector<string> in = {"child"};
    auto [c, args, err] = rootCmd->Traverse(in);
    EXPECT_FALSE(err) << err;
    EXPECT_TRUE(args.empty());
    EXPECT_EQ(c->Name(), childCmd->Name());
}

TEST(GoCmd, TraverseWithBadParentFlags) {
    auto rootCmd = cmd::New({.Use = "root", .TraverseChildren = true});
    auto childCmd = cmd::New({.Use = "child"});
    childCmd->Flags()->String("str", "", "");
    rootCmd->AddCommand(childCmd);

    std::vector<string> in = {"--str", "ok", "child"};
    auto [c, _, err] = rootCmd->Traverse(in);
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "unknown flag: --str");
    EXPECT_EQ(c, nullptr);
}

TEST(GoCmd, TraverseWithBadChildFlag) {
    auto rootCmd = cmd::New({.Use = "root", .TraverseChildren = true});
    rootCmd->Flags()->String("str", "", "");
    auto childCmd = cmd::New({.Use = "child"});
    rootCmd->AddCommand(childCmd);

    // Expect no error because the last commands args shouldn't be parsed
    // in Traverse.
    std::vector<string> in = {"child", "--str"};
    auto [c, args, err] = rootCmd->Traverse(in);
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(args, std::vector<string>{"--str"});
    EXPECT_EQ(c->Name(), childCmd->Name());
}

TEST(GoCmd, TraverseWithTwoSubcommands) {
    auto rootCmd = cmd::New({.Use = "root", .TraverseChildren = true});
    auto subCmd = cmd::New({.Use = "sub", .TraverseChildren = true});
    rootCmd->AddCommand(subCmd);
    auto subsubCmd = cmd::New({.Use = "subsub"});
    subCmd->AddCommand(subsubCmd);

    std::vector<string> in = {"sub", "subsub"};
    auto [c, _, err] = rootCmd->Traverse(in);
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(c->Name(), subsubCmd->Name());
}

TEST(GoCmd, UpdateName) {
    auto c = cmd::New({.Use = "name xyz"});
    std::string originalName = c->Name().str();
    c->Use = "changedName abc";
    EXPECT_NE(originalName, c->Name()) << "c.Name() should be updated on changed c.Use";
}

TEST(GoCmd, CalledAs) {
    struct calledAsTestcase {
        std::string name;
        std::vector<string> args;
        std::string call;
        std::string want;
        bool epm;
    };
    calledAsTestcase tests[] = {
        {"find/no-args", {}, "parent", "parent", false},
        {"find/real-name", {"child1"}, "child1", "child1", false},
        {"find/full-alias", {"that"}, "child2", "that", false},
        {"find/part-no-prefix", {"thi"}, "", "", false},
        {"find/part-alias", {"thi"}, "child1", "this", true},
        {"find/conflict", {"th"}, "", "", true},
    };
    restore r(cmd::EnablePrefixMatching);
    for (auto& tc : tests) {
        SCOPED_TRACE(tc.name);
        cmd::EnablePrefixMatching = tc.epm;
        Command* called = nullptr;
        auto run = [&](Command& c, std::span<const string>) { called = &c; };
        auto parent = cmd::New({.Use = "parent", .Run = run});
        auto child1 = cmd::New({.Use = "child1", .Aliases = {"this"}, .Run = run});
        auto child2 = cmd::New({.Use = "child2", .Aliases = {"that"}, .Run = run});
        parent->AddCommand(child1);
        parent->AddCommand(child2);
        auto output = make_ptr<bytes::Buffer>();
        parent->SetArgs(tc.args);
        parent->SetOut(output);
        parent->SetErr(output);
        parent->Execute();
        if (!called) {
            EXPECT_EQ(tc.call, "") << "missing expected call to command";
            continue;
        }
        EXPECT_EQ(called->Name(), tc.call);
        EXPECT_EQ(called->CalledAs(), tc.want);
    }
}

TEST(GoCmd, FParseErrWhitelistBackwardCompatibility) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun});
    c->Flags()->BoolP("boola", "a", false, "a boolean flag");
    auto [output, err] = executeCommand(*c, {"c", "-a", "--unknown", "flag"});
    ASSERT_TRUE(err) << "expected unknown flag error";
    checkStringContains(output, "unknown flag: --unknown");
}

TEST(GoCmd, FParseErrWhitelistSameCommand) {
    auto c = cmd::New({.Use = "c", .Run = emptyRun, .FParseErrWhitelist = {.UnknownFlags = true}});
    c->Flags()->BoolP("boola", "a", false, "a boolean flag");
    auto [_, err] = executeCommand(*c, {"c", "-a", "--unknown", "flag"});
    EXPECT_FALSE(err) << "unexpected error: " << err;
}

TEST(GoCmd, FParseErrWhitelistParentCommand) {
    auto root = cmd::New({.Use = "root", .Run = emptyRun, .FParseErrWhitelist = {.UnknownFlags = true}});
    auto c = cmd::New({.Use = "child", .Run = emptyRun});
    c->Flags()->BoolP("boola", "a", false, "a boolean flag");
    root->AddCommand(c);
    auto [output, err] = executeCommand(*root, {"child", "-a", "--unknown", "flag"});
    EXPECT_TRUE(err) << "expected unknown flag error";
    checkStringContains(output, "unknown flag: --unknown");
}

TEST(GoCmd, FParseErrWhitelistChildCommand) {
    auto root = cmd::New({.Use = "root", .Run = emptyRun});
    auto c = cmd::New({.Use = "child", .Run = emptyRun, .FParseErrWhitelist = {.UnknownFlags = true}});
    c->Flags()->BoolP("boola", "a", false, "a boolean flag");
    root->AddCommand(c);
    auto [_, err] = executeCommand(*root, {"child", "-a", "--unknown", "flag"});
    EXPECT_FALSE(err) << "unexpected error: " << err;
}

TEST(GoCmd, SetContextPreRun) {
    static int key;
    auto val = make_ptr<bytes::Buffer>();
    auto root = cmd::New({
        .Use = "root",
        .PreRun = [&](Command& cmd, std::span<const string>) { cmd.SetContext(context::WithValue(cmd.Context(), &key, val)); },
        .Run = [&](Command& cmd, std::span<const string>) { EXPECT_EQ(cmd.Context()->Value(&key), ptr<object>(val)) << "key not found in context"; },
    });
    root->SetArgs({});
    EXPECT_FALSE(root->Execute());
}

TEST(GoCmd, NoRootRunCommandExecutedWithVersionSet) {
    auto rootCmd = cmd::New({.Use = "root", .Long = "Long description", .Version = "1.0.0"});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd);
    EXPECT_FALSE(err) << err;
    checkStringContains(output, rootCmd->Long.str());
    checkStringContains(output, "help for root");
    checkStringContains(output, "version for root");
}

TEST(GoCmd, HelpCommandExecutedWithoutVersionSet) {
    auto rootCmd = cmd::New({.Use = "root", .Long = "Long description"});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [output, err] = executeCommand(*rootCmd, {"help"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, rootCmd->Long.str());
    checkStringContains(output, "help for root");
    checkStringOmits(output, "version for root");
}

TEST(GoCmd, Find) {
    string foo, bar;
    auto root = cmd::New({.Use = "root"});
    root->PersistentFlags()->StringVarP(&foo, "foo", "f", "", "");
    root->PersistentFlags()->StringVarP(&bar, "bar", "b", "something", "");
    auto child = cmd::New({.Use = "child"});
    root->AddCommand(child);

    struct {
        std::vector<string> args;
        std::vector<string> expectedFoundArgs;
    } testCases[] = {
        {{"child"}, {}},
        {{"child", "child"}, {"child"}},
        {{"child", "foo", "child", "bar", "child", "baz", "child"}, {"foo", "child", "bar", "child", "baz", "child"}},
        {{"-f", "child", "child"}, {"-f", "child"}},
        {{"child", "-f", "child"}, {"-f", "child"}},
        {{"-b", "child", "child"}, {"-b", "child"}},
        {{"child", "-b", "child"}, {"-b", "child"}},
        {{"child", "-b"}, {"-b"}},
        {{"-b", "-f", "child", "child"}, {"-b", "-f", "child"}},
        {{"-f", "child", "-b", "something", "child"}, {"-f", "child", "-b", "something"}},
        {{"-f", "child", "child", "-b"}, {"-f", "child", "-b"}},
        {{"-f=child", "-b=something", "child"}, {"-f=child", "-b=something"}},
        {{"--foo", "child", "--bar", "something", "child"}, {"--foo", "child", "--bar", "something"}},
    };
    for (auto& tc : testCases) {
        SCOPED_TRACE(join(tc.args));
        auto [c, foundArgs, err] = root->Find(tc.args);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(c, child.get()) << "Expected cmd to be child, but it was not";
        EXPECT_EQ(tc.expectedFoundArgs, foundArgs);
    }
}

TEST(GoCmd, UnknownFlagShouldReturnSameErrorRegardlessOfArgPosition) {
    std::vector<std::vector<string>> testCases = {
        {"--namespace", "foo", "--unknown", "child", "--bar"},
        {"--namespace", "foo", "child", "--unknown", "--bar"},
        {"--namespace", "foo", "child", "--bar", "--unknown"},
        {"--unknown", "--namespace=foo", "child", "--bar"},
        {"--namespace=foo", "--unknown", "child", "--bar"},
        {"--namespace=foo", "child", "--unknown", "--bar"},
        {"--namespace=foo", "child", "--bar", "--unknown"},
        {"--unknown", "--namespace=foo", "child", "--bar=true"},
        {"--namespace=foo", "--unknown", "child", "--bar=true"},
        {"--namespace=foo", "child", "--unknown", "--bar=true"},
        {"--namespace=foo", "child", "--bar=true", "--unknown"},
    };
    auto root = cmd::New({.Use = "root", .Run = emptyRun});
    root->PersistentFlags()->String("namespace", "", "a string flag");
    auto c = cmd::New({.Use = "child", .Run = emptyRun});
    c->Flags()->Bool("bar", false, "a boolean flag");
    root->AddCommand(c);
    for (auto& tc : testCases) {
        SCOPED_TRACE(join(tc));
        auto [output, err] = executeCommand(*root, tc);
        EXPECT_TRUE(err) << "expected unknown flag error";
        checkStringContains(output, "unknown flag: --unknown");
    }
}

TEST(GoCmd, HelpFuncExecuted) {
    std::string helpText = "Long description";
    static int testKey;
    auto executionCtx = context::WithValue(context::Background(), &testKey, make_ptr<bytes::Buffer>());

    auto child = cmd::New({.Use = "child", .Run = emptyRun});
    child->SetHelpFunc([&](Command& cmd, std::span<const string>) {
        io::WriteString(cmd.OutOrStdout(), helpText);
        EXPECT_EQ(cmd.Context(), executionCtx) << "Context doesn't equal the execution context";
    });
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->AddCommand(child);

    auto [output, err] = executeCommandWithContext(executionCtx, *rootCmd, {"help", "child"});
    EXPECT_FALSE(err) << err;
    checkStringContains(output, helpText);
}

// A help template that uses the Command's methods and fields, and cobra's
// template functions, as kubectl's do.
TEST(GoCmd, TemplateOverCommand) {
    auto rootCmd = cmd::New({.Use = "root", .Short = "the root", .Run = emptyRun});
    rootCmd->CompletionOptions.DisableDefaultCmd = true;
    auto a = cmd::New({.Use = "alpha", .Aliases = {"a"}, .Short = "first", .Run = emptyRun});
    auto b = cmd::New({.Use = "beta", .Short = "second", .Run = emptyRun});
    rootCmd->AddCommand(a, b);
    rootCmd->Flags()->StringP("name", "n", "x", "a name");
    rootCmd->SetHelpTemplate(
        "{{.Short | trim}}|{{range .Commands}}{{if .IsAvailableCommand}}[{{rpad .Name 6}}{{.NameAndAliases}}]{{end}}{{end}}|"
        "{{.LocalFlags.FlagUsages | trimTrailingWhitespaces}}|{{gt (len .Aliases) 0}}|{{eq .Name \"root\"}}|{{appendIfNotPresent .Use \"[x]\"}}");
    auto [output, err] = executeCommand(*rootCmd, {"--help"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, "the root|[alpha alpha, a][beta  beta]|  -h, --help          help for root\n  -n, --name string   a name (default \"x\")|false|true|root [x]");
}

// ---------------------------------------------------------------------------
// args_test.go

namespace {

ptr<Command> getCommand(cmd::PositionalArgs args, bool withValid) {
    auto c = cmd::New({.Use = "c", .Args = std::move(args), .Run = emptyRun});
    if (withValid) c->ValidArgs = {"one", "two", "three"};
    return c;
}

void expectSuccess(const std::string& output, const error& err) {
    EXPECT_EQ(output, "") << "Unexpected output";
    EXPECT_FALSE(err) << "Unexpected error: " << err;
}

void expectErr(const error& err, const std::string& expected) {
    ASSERT_TRUE(err) << "Expected an error";
    EXPECT_EQ(err.Error(), expected);
}

const std::string validOnlyWithInvalidArgs = "invalid argument \"a\" for \"c\"";
const std::string minimumNArgsWithLessArgs = "requires at least 2 arg(s), only received 1";
const std::string maximumNArgsWithMoreArgs = "accepts at most 2 arg(s), received 3";
const std::string exactArgsWithInvalidCount = "accepts 2 arg(s), received 3";
const std::string rangeArgsWithInvalidCount = "accepts between 2 and 4 arg(s), received 1";

std::string noArgsWithArgs(std::string_view arg) { return std::format("unknown command \"{}\" for \"c\"", arg); }

}  // namespace

TEST(GoCmd, NoArgs) {
    auto c = getCommand(cmd::NoArgs, false);
    auto [output, err] = executeCommand(*c);
    expectSuccess(output, err);
}

TEST(GoCmd, NoArgs_WithArgs) {
    auto c = getCommand(cmd::NoArgs, false);
    expectErr(executeCommand(*c, {"one"}).second, noArgsWithArgs("one"));
}

TEST(GoCmd, NoArgs_WithValid_WithArgs) {
    auto c = getCommand(cmd::NoArgs, true);
    expectErr(executeCommand(*c, {"one"}).second, noArgsWithArgs("one"));
}

TEST(GoCmd, NoArgs_WithValid_WithInvalidArgs) {
    auto c = getCommand(cmd::NoArgs, true);
    expectErr(executeCommand(*c, {"a"}).second, noArgsWithArgs("a"));
}

TEST(GoCmd, NoArgs_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::NoArgs), true);
    expectErr(executeCommand(*c, {"a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, OnlyValidArgs) {
    auto c = getCommand(cmd::OnlyValidArgs, true);
    auto [output, err] = executeCommand(*c, {"one", "two"});
    expectSuccess(output, err);
}

TEST(GoCmd, OnlyValidArgs_WithInvalidArgs) {
    auto c = getCommand(cmd::OnlyValidArgs, true);
    expectErr(executeCommand(*c, {"a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, ArbitraryArgs) {
    auto c = getCommand(cmd::ArbitraryArgs, false);
    auto [output, err] = executeCommand(*c, {"a", "b"});
    expectSuccess(output, err);
}

TEST(GoCmd, ArbitraryArgs_WithValid) {
    auto c = getCommand(cmd::ArbitraryArgs, true);
    auto [output, err] = executeCommand(*c, {"one", "two"});
    expectSuccess(output, err);
}

TEST(GoCmd, ArbitraryArgs_WithValid_WithInvalidArgs) {
    auto c = getCommand(cmd::ArbitraryArgs, true);
    auto [output, err] = executeCommand(*c, {"a"});
    expectSuccess(output, err);
}

TEST(GoCmd, ArbitraryArgs_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::ArbitraryArgs), true);
    expectErr(executeCommand(*c, {"a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, MinimumNArgs) {
    auto c = getCommand(cmd::MinimumNArgs(2), false);
    auto [output, err] = executeCommand(*c, {"a", "b", "c"});
    expectSuccess(output, err);
}

TEST(GoCmd, MinimumNArgs_WithValid) {
    auto c = getCommand(cmd::MinimumNArgs(2), true);
    auto [output, err] = executeCommand(*c, {"one", "three"});
    expectSuccess(output, err);
}

TEST(GoCmd, MinimumNArgs_WithValid__WithInvalidArgs) {
    auto c = getCommand(cmd::MinimumNArgs(2), true);
    auto [output, err] = executeCommand(*c, {"a", "b"});
    expectSuccess(output, err);
}

TEST(GoCmd, MinimumNArgs_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::MinimumNArgs(2)), true);
    expectErr(executeCommand(*c, {"a", "b"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, MinimumNArgs_WithLessArgs) {
    auto c = getCommand(cmd::MinimumNArgs(2), false);
    expectErr(executeCommand(*c, {"a"}).second, minimumNArgsWithLessArgs);
}

TEST(GoCmd, MinimumNArgs_WithLessArgs_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::MinimumNArgs(2)), true);
    expectErr(executeCommand(*c, {"a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, MaximumNArgs) {
    auto c = getCommand(cmd::MaximumNArgs(3), false);
    auto [output, err] = executeCommand(*c, {"a", "b"});
    expectSuccess(output, err);
}

TEST(GoCmd, MaximumNArgs_WithMoreArgs) {
    auto c = getCommand(cmd::MaximumNArgs(2), false);
    expectErr(executeCommand(*c, {"a", "b", "c"}).second, maximumNArgsWithMoreArgs);
}

TEST(GoCmd, MaximumNArgs_WithMoreArgs_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::MaximumNArgs(2)), true);
    expectErr(executeCommand(*c, {"a", "b", "c"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, ExactArgs) {
    auto c = getCommand(cmd::ExactArgs(3), false);
    auto [output, err] = executeCommand(*c, {"a", "b", "c"});
    expectSuccess(output, err);
}

TEST(GoCmd, ExactArgs_WithInvalidCount) {
    auto c = getCommand(cmd::ExactArgs(2), false);
    expectErr(executeCommand(*c, {"a", "b", "c"}).second, exactArgsWithInvalidCount);
}

TEST(GoCmd, ExactArgs_WithInvalidCount_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::ExactArgs(2)), true);
    expectErr(executeCommand(*c, {"three", "one", "a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, RangeArgs) {
    auto c = getCommand(cmd::RangeArgs(2, 4), false);
    auto [output, err] = executeCommand(*c, {"a", "b", "c"});
    expectSuccess(output, err);
}

TEST(GoCmd, RangeArgs_WithInvalidCount) {
    auto c = getCommand(cmd::RangeArgs(2, 4), false);
    expectErr(executeCommand(*c, {"a"}).second, rangeArgsWithInvalidCount);
}

TEST(GoCmd, RangeArgs_WithInvalidCount_WithValidOnly_WithInvalidArgs) {
    auto c = getCommand(cmd::MatchAll(cmd::OnlyValidArgs, cmd::RangeArgs(2, 4)), true);
    expectErr(executeCommand(*c, {"a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, RootTakesNoArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [_, err] = executeCommand(*rootCmd, {"illegal", "args"});
    ASSERT_TRUE(err) << "Expected an error";
    checkStringContains(err.Error().str(), "unknown command \"illegal\" for \"root\"");
}

TEST(GoCmd, RootTakesArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::ArbitraryArgs, .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Run = emptyRun}));
    auto [_, err] = executeCommand(*rootCmd, {"legal", "args"});
    EXPECT_FALSE(err) << err;
}

TEST(GoCmd, ChildTakesNoArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Args = cmd::NoArgs, .Run = emptyRun}));
    auto [_, err] = executeCommand(*rootCmd, {"child", "illegal", "args"});
    ASSERT_TRUE(err) << "Expected an error";
    checkStringContains(err.Error().str(), "unknown command \"illegal\" for \"root child\"");
}

TEST(GoCmd, ChildTakesArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->AddCommand(cmd::New({.Use = "child", .Args = cmd::ArbitraryArgs, .Run = emptyRun}));
    auto [_, err] = executeCommand(*rootCmd, {"child", "legal", "args"});
    EXPECT_FALSE(err) << err;
}

TEST(GoCmd, MatchAll) {
    // Somewhat contrived example check that ensures there are exactly 3
    // arguments, and each argument is exactly 2 bytes long.
    auto pargs = cmd::MatchAll(cmd::ExactArgs(3), [](Command&, std::span<const string> args) -> error {
        for (auto& arg : args) {
            if (arg.size() != 2) return fmt::Errorf("expected to be exactly 2 bytes long");
        }
        return nullptr;
    });
    struct {
        std::string name;
        std::vector<string> args;
        bool fail;
    } testCases[] = {
        {"happy path", {"aa", "bb", "cc"}, false},
        {"incorrect number of args", {"aa", "bb", "cc", "dd"}, true},
        {"incorrect number of bytes in one arg", {"aa", "bb", "abc"}, true},
    };
    auto rootCmd = cmd::New({.Use = "root", .Args = pargs, .Run = emptyRun});
    for (auto& tc : testCases) {
        auto [_, err] = executeCommand(*rootCmd, tc.args);
        EXPECT_EQ(static_cast<bool>(err), tc.fail) << tc.name << ": " << err;
    }
}

TEST(GoCmd, ExactValidArgs) {
    auto c = getCommand(cmd::ExactValidArgs(3), true);
    auto [output, err] = executeCommand(*c, {"three", "one", "two"});
    expectSuccess(output, err);
}

TEST(GoCmd, ExactValidArgs_WithInvalidArgs) {
    auto c = getCommand(cmd::ExactValidArgs(2), true);
    expectErr(executeCommand(*c, {"three", "a"}).second, validOnlyWithInvalidArgs);
}

TEST(GoCmd, LegacyArgsRootAcceptsArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto [_, err] = executeCommand(*rootCmd, {"somearg"});
    EXPECT_FALSE(err) << err;
}

TEST(GoCmd, LegacyArgsSubcmdAcceptsArgs) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    auto grandchildCmd = cmd::New({.Use = "grandchild", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    childCmd->AddCommand(grandchildCmd);
    auto [_, err] = executeCommand(*rootCmd, {"child", "somearg"});
    EXPECT_FALSE(err) << err;
}

// ---------------------------------------------------------------------------
// flag_groups_test.go

TEST(GoCmd, ValidateFlagGroups) {
    auto getCmd = [] {
        auto c = cmd::New({.Use = "testcmd", .Run = emptyRun});
        // Define lots of flags to utilize for testing.
        for (auto v : {"a", "b", "c", "d"}) c->Flags()->String(v, "", "");
        for (auto v : {"e", "f", "g"}) c->PersistentFlags()->String(v, "", "");
        auto subC = cmd::New({.Use = "subcmd", .Run = emptyRun});
        subC->Flags()->String("subonly", "", "");
        c->AddCommand(subC);
        return c;
    };
    using sv = std::vector<std::string>;
    struct {
        std::string desc{};
        sv flagGroupsRequired{}, flagGroupsOneRequired{}, flagGroupsExclusive{};
        sv subCmdFlagGroupsRequired{}, subCmdFlagGroupsOneRequired{}, subCmdFlagGroupsExclusive{};
        std::vector<string> args{};
        std::string expectErr{};
    } testcases[] = {
        {.desc = "No flags no problem"},
        {.desc = "No flags no problem even with conflicting groups", .flagGroupsRequired = {"a b"}, .flagGroupsExclusive = {"a b"}},
        {.desc = "Required flag group not satisfied",
         .flagGroupsRequired = {"a b c"},
         .args = {"--a=foo"},
         .expectErr = "if any flags in the group [a b c] are set they must all be set; missing [b c]"},
        {.desc = "One-required flag group not satisfied",
         .flagGroupsOneRequired = {"a b"},
         .args = {"--c=foo"},
         .expectErr = "at least one of the flags in the group [a b] is required"},
        {.desc = "Exclusive flag group not satisfied",
         .flagGroupsExclusive = {"a b c"},
         .args = {"--a=foo", "--b=foo"},
         .expectErr = "if any flags in the group [a b c] are set none of the others can be; [a b] were all set"},
        {.desc = "Multiple required flag group not satisfied returns first error",
         .flagGroupsRequired = {"a b c", "a d"},
         .args = {"--c=foo", "--d=foo"},
         .expectErr = "if any flags in the group [a b c] are set they must all be set; missing [a b]"},
        {.desc = "Multiple one-required flag group not satisfied returns first error",
         .flagGroupsOneRequired = {"a b", "d e"},
         .args = {"--c=foo", "--f=foo"},
         .expectErr = "at least one of the flags in the group [a b] is required"},
        {.desc = "Multiple exclusive flag group not satisfied returns first error",
         .flagGroupsExclusive = {"a b c", "a d"},
         .args = {"--a=foo", "--c=foo", "--d=foo"},
         .expectErr = "if any flags in the group [a b c] are set none of the others can be; [a c] were all set"},
        {.desc = "Validation of required groups occurs on groups in sorted order",
         .flagGroupsRequired = {"a d", "a b", "a c"},
         .args = {"--a=foo"},
         .expectErr = "if any flags in the group [a b] are set they must all be set; missing [b]"},
        {.desc = "Validation of one-required groups occurs on groups in sorted order",
         .flagGroupsOneRequired = {"d e", "a b", "f g"},
         .args = {"--c=foo"},
         .expectErr = "at least one of the flags in the group [a b] is required"},
        {.desc = "Validation of exclusive groups occurs on groups in sorted order",
         .flagGroupsExclusive = {"a d", "a b", "a c"},
         .args = {"--a=foo", "--b=foo", "--c=foo"},
         .expectErr = "if any flags in the group [a b] are set none of the others can be; [a b] were all set"},
        {.desc = "Persistent flags utilize required and exclusive groups and can fail required groups",
         .flagGroupsRequired = {"a e", "e f"},
         .flagGroupsExclusive = {"f g"},
         .args = {"--a=foo", "--f=foo", "--g=foo"},
         .expectErr = "if any flags in the group [a e] are set they must all be set; missing [e]"},
        {.desc = "Persistent flags utilize one-required and exclusive groups and can fail one-required groups",
         .flagGroupsOneRequired = {"a b", "e f"},
         .flagGroupsExclusive = {"e f"},
         .args = {"--e=foo"},
         .expectErr = "at least one of the flags in the group [a b] is required"},
        {.desc = "Persistent flags utilize required and exclusive groups and can fail mutually exclusive groups",
         .flagGroupsRequired = {"a e", "e f"},
         .flagGroupsExclusive = {"f g"},
         .args = {"--a=foo", "--e=foo", "--f=foo", "--g=foo"},
         .expectErr = "if any flags in the group [f g] are set none of the others can be; [f g] were all set"},
        {.desc = "Persistent flags utilize required and exclusive groups and can pass",
         .flagGroupsRequired = {"a e", "e f"},
         .flagGroupsExclusive = {"f g"},
         .args = {"--a=foo", "--e=foo", "--f=foo"}},
        {.desc = "Persistent flags utilize one-required and exclusive groups and can pass",
         .flagGroupsOneRequired = {"a e", "e f"},
         .flagGroupsExclusive = {"f g"},
         .args = {"--a=foo", "--e=foo", "--f=foo"}},
        {.desc = "Subcmds can use required groups using inherited flags",
         .subCmdFlagGroupsRequired = {"e subonly"},
         .args = {"subcmd", "--e=foo", "--subonly=foo"}},
        {.desc = "Subcmds can use one-required groups using inherited flags",
         .subCmdFlagGroupsOneRequired = {"e subonly"},
         .args = {"subcmd", "--e=foo", "--subonly=foo"}},
        {.desc = "Subcmds can use one-required groups using inherited flags and fail one-required groups",
         .subCmdFlagGroupsOneRequired = {"e subonly"},
         .args = {"subcmd"},
         .expectErr = "at least one of the flags in the group [e subonly] is required"},
        {.desc = "Subcmds can use exclusive groups using inherited flags",
         .subCmdFlagGroupsExclusive = {"e subonly"},
         .args = {"subcmd", "--e=foo", "--subonly=foo"},
         .expectErr = "if any flags in the group [e subonly] are set none of the others can be; [e subonly] were all set"},
        {.desc = "Subcmds can use exclusive groups using inherited flags and pass",
         .subCmdFlagGroupsExclusive = {"e subonly"},
         .args = {"subcmd", "--e=foo"}},
        {.desc = "Flag groups not applied if not found on invoked command",
         .subCmdFlagGroupsRequired = {"e subonly"},
         .args = {"--e=foo"}},
    };
    for (auto& tc : testcases) {
        SCOPED_TRACE(tc.desc);
        auto c = getCmd();
        auto sub = c->Commands()[0];
        for (auto& g : tc.flagGroupsRequired) c->MarkFlagsRequiredTogether(strings::Split(g, " "));
        for (auto& g : tc.flagGroupsOneRequired) c->MarkFlagsOneRequired(strings::Split(g, " "));
        for (auto& g : tc.flagGroupsExclusive) c->MarkFlagsMutuallyExclusive(strings::Split(g, " "));
        for (auto& g : tc.subCmdFlagGroupsRequired) sub->MarkFlagsRequiredTogether(strings::Split(g, " "));
        for (auto& g : tc.subCmdFlagGroupsOneRequired) sub->MarkFlagsOneRequired(strings::Split(g, " "));
        for (auto& g : tc.subCmdFlagGroupsExclusive) sub->MarkFlagsMutuallyExclusive(strings::Split(g, " "));
        c->SetArgs(tc.args);
        c->SetOut(make_ptr<bytes::Buffer>());
        c->SetErr(make_ptr<bytes::Buffer>());
        auto err = c->Execute();
        if (tc.expectErr.empty()) {
            EXPECT_FALSE(err) << err;
        } else {
            ASSERT_TRUE(err) << "Expected error " << tc.expectErr;
            EXPECT_EQ(err.Error(), tc.expectErr);
        }
    }
}

// ---------------------------------------------------------------------------
// completions_test.go

TEST(GoCmd, CmdNameCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd1 = cmd::New({.Use = "firstChild", .Short = "First command", .Run = emptyRun});
    auto childCmd2 = cmd::New({.Use = "secondChild", .Run = emptyRun});
    auto hiddenCmd = cmd::New({.Use = "testHidden", .Run = emptyRun, .Hidden = true});
    auto deprecatedCmd = cmd::New({.Use = "testDeprecated", .Deprecated = "deprecated", .Run = emptyRun});
    auto aliasedCmd = cmd::New({.Use = "aliased", .Aliases = {"testAlias", "testSynonym"}, .Short = "A command with aliases", .Run = emptyRun});
    rootCmd->AddCommand(childCmd1, childCmd2, hiddenCmd, deprecatedCmd, aliasedCmd);

    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);
    // Test that sub-command names are completed.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"aliased", "completion", "firstChild", "help", "secondChild", ":4",
                             "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that sub-command names are completed with prefix.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "s"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"secondChild", ":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that even with no valid sub-command matches, hidden, deprecated
    // and aliases are not completed.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "test"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that sub-command names are completed with description.
    std::tie(output, err) = executeCommand(*rootCmd, {string::fromStatic(cmd::ShellCompRequestCmd), ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"aliased\tA command with aliases", "completion\tGenerate the autocompletion script for the specified shell",
                             "firstChild\tFirst command", "help\tHelp about any command", "secondChild", ":4",
                             "Completion ended with directive: ShellCompDirectiveNoFileComp"}));
}

TEST(GoCmd, ValidArgsCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .ValidArgs = {"one", "two", "three"}, .Args = cmd::MinimumNArgs(1)});
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);

    // Test that validArgs are completed.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"one", "two", "three", ":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that validArgs are completed with prefix.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "o"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"one", ":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that validArgs don't repeat.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "one", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({":0", "Completion ended with directive: ShellCompDirectiveDefault"}));
}

TEST(GoCmd, FlagNameCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "childCmd", .Version = "1.2.3", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->Flags()->IntP("first", "f", -1, "first flag");
    rootCmd->PersistentFlags()->BoolP("second", "s", false, "second flag");
    childCmd->Flags()->String("subFlag", "", "sub flag");
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);

    // Test that flag names are not shown if the user has not given the '-'
    // prefix.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"childCmd", "completion", "help", ":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that flag names are completed.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--first", "-f", "--help", "-h", "--second", "-s", ":4",
                             "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that flag names are completed when a prefix is given.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--f"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--first", ":4", "Completion ended with directive: ShellCompDirectiveNoFileComp"}));

    // Test that flag names are completed in a sub-cmd.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "childCmd", "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--second", "-s", "--help", "-h", "--subFlag", "--version", "-v", ":4",
                             "Completion ended with directive: ShellCompDirectiveNoFileComp"}));
}

TEST(GoCmd, RequiredFlagNameCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .ValidArgs = {"realArg"}, .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "childCmd",
                              .ValidArgsFunction = cmd::FixedCompletions({"subArg"}, cmd::ShellCompDirectiveNoFileComp),
                              .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->Flags()->IntP("requiredFlag", "r", -1, "required flag");
    EXPECT_FALSE(rootCmd->MarkFlagRequired("requiredFlag"));
    auto* requiredFlag = rootCmd->Flags()->Lookup("requiredFlag");
    rootCmd->PersistentFlags()->IntP("requiredPersistent", "p", -1, "required persistent");
    EXPECT_FALSE(rootCmd->MarkPersistentFlagRequired("requiredPersistent"));
    auto* requiredPersistent = rootCmd->PersistentFlags()->Lookup("requiredPersistent");
    rootCmd->Flags()->StringP("release", "R", "", "Release name");
    childCmd->Flags()->BoolP("subRequired", "s", false, "sub required flag");
    EXPECT_FALSE(childCmd->MarkFlagRequired("subRequired"));
    childCmd->Flags()->BoolP("subNotRequired", "n", false, "sub not required flag");
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);
    std::string_view nofile = "Completion ended with directive: ShellCompDirectiveNoFileComp";

    // Test that a required flag is suggested even without the - prefix.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"childCmd", "completion", "help", "--requiredFlag", "-r", "--requiredPersistent", "-p", "realArg", ":4", nofile}));

    // Test that a required flag is suggested without other flags when using
    // the '-' prefix.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--requiredFlag", "-r", "--requiredPersistent", "-p", ":4", nofile}));

    // Test that if no required flag matches, the normal flags are suggested.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--relea"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--release", ":4", nofile}));

    // Test required flags for sub-commands.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "childCmd", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--requiredPersistent", "-p", "--subRequired", "-s", "subArg", ":4", nofile}));

    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "childCmd", "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--requiredPersistent", "-p", "--subRequired", "-s", ":4", nofile}));

    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "childCmd", "--subNot"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--subNotRequired", ":4", nofile}));

    // Test that when a required flag is present, it is not suggested
    // anymore.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--requiredFlag", "1", ""});
    EXPECT_FALSE(err) << err;
    requiredFlag->Changed = false;
    EXPECT_EQ(output, lines({"--requiredPersistent", "-p", "realArg", ":4", nofile}));

    // Test that when a persistent required flag is present, it is not
    // suggested anymore.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--requiredPersistent", "1", ""});
    EXPECT_FALSE(err) << err;
    requiredPersistent->Changed = false;
    EXPECT_EQ(output, lines({"childCmd", "completion", "help", "--requiredFlag", "-r", "realArg", ":4", nofile}));

    // Test that when all required flags are present, normal completion is
    // done.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--requiredFlag", "1", "--requiredPersistent", "1", ""});
    EXPECT_FALSE(err) << err;
    requiredFlag->Changed = false;
    requiredPersistent->Changed = false;
    EXPECT_EQ(output, lines({"realArg", ":4", nofile}));
}

TEST(GoCmd, FlagCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->Flags()->IntP("introot", "i", -1, "help message for flag introot");
    EXPECT_FALSE(rootCmd->RegisterFlagCompletionFunc("introot", [](Command&, std::span<const string>, std::string_view toComplete) {
        std::vector<cmd::Completion> completions;
        for (auto comp : {"1\tThe first", "2\tThe second", "10\tThe tenth"}) {
            if (std::string_view(comp).starts_with(toComplete)) completions.push_back(comp);
        }
        return std::pair{completions, cmd::ShellCompDirectiveDefault};
    }));
    rootCmd->Flags()->String("filename", "", "Enter a filename");
    EXPECT_FALSE(rootCmd->RegisterFlagCompletionFunc("filename", [](Command&, std::span<const string>, std::string_view toComplete) {
        std::vector<cmd::Completion> completions;
        for (auto comp : {"file.yaml\tYAML format", "myfile.json\tJSON format", "file.xml\tXML format"}) {
            if (std::string_view(comp).starts_with(toComplete)) completions.push_back(comp);
        }
        return std::pair{completions, cmd::ShellCompDirectiveNoSpace | cmd::ShellCompDirectiveNoFileComp};
    }));
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);

    // Test completing an empty string.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, "--introot", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"1", "2", "10", ":0", "Completion ended with directive: ShellCompDirectiveDefault"}));

    // Check completing with a prefix.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--introot", "1"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"1", "10", ":0", "Completion ended with directive: ShellCompDirectiveDefault"}));

    // Test completing an empty string.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--filename", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"file.yaml", "myfile.json", "file.xml", ":6",
                             "Completion ended with directive: ShellCompDirectiveNoSpace, ShellCompDirectiveNoFileComp"}));

    // Check completing with a prefix.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--filename", "f"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"file.yaml", "file.xml", ":6",
                             "Completion ended with directive: ShellCompDirectiveNoSpace, ShellCompDirectiveNoFileComp"}));
}

TEST(GoCmd, CompleteHelp) {
    auto rootCmd = cmd::New({.Use = "root", .Args = cmd::NoArgs, .Run = emptyRun});
    auto child1Cmd = cmd::New({.Use = "child1", .Run = emptyRun});
    auto child2Cmd = cmd::New({.Use = "child2", .Run = emptyRun});
    rootCmd->AddCommand(child1Cmd, child2Cmd);
    auto child3Cmd = cmd::New({.Use = "child3", .Run = emptyRun});
    child1Cmd->AddCommand(child3Cmd);
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);
    std::string_view nofile = "Completion ended with directive: ShellCompDirectiveNoFileComp";

    // Test that completion includes the help command.
    auto [output, err] = executeCommand(*rootCmd, {noDesc, ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"child1", "child2", "completion", "help", ":4", nofile}));

    // Test sub-commands are completed on first level of help command.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "help", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"child1", "child2", "completion", "help", ":4", nofile}));

    // Test sub-commands are completed on first level of help command.
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "help", "child1", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"child3", ":4", nofile}));
}

TEST(GoCmd, FlagDirFilterCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->Flags()->String("dir", "", "");
    EXPECT_FALSE(rootCmd->MarkFlagDirname("dir"));
    rootCmd->Flags()->String("subdir", "", "");
    EXPECT_FALSE(rootCmd->Flags()->SetAnnotation("subdir", cmd::BashCompSubdirsInDir, {"themes"}));
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);

    auto [output, err] = executeCommand(*rootCmd, {noDesc, "--dir", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({":16", "Completion ended with directive: ShellCompDirectiveFilterDirs"}));

    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "--subdir", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"themes", ":16", "Completion ended with directive: ShellCompDirectiveFilterDirs"}));
}

TEST(GoCmd, FlagFileExtFilterCompletionInGo) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    rootCmd->Flags()->String("file", "", "");
    EXPECT_FALSE(rootCmd->MarkFlagFilename("file", "yaml", "json"));
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);

    auto [output, err] = executeCommand(*rootCmd, {noDesc, "--file", ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"yaml", "json", ":8", "Completion ended with directive: ShellCompDirectiveFilterFileExt"}));
}

TEST(GoCmd, CompletionForMutuallyExclusiveFlags) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    auto childCmd = cmd::New({.Use = "child", .Run = emptyRun});
    rootCmd->AddCommand(childCmd);
    rootCmd->PersistentFlags()->Int("ingroup1", -1, "ingroup1");
    rootCmd->PersistentFlags()->String("only-if-all", "", "only-if-all");
    childCmd->Flags()->Int("ingroup2", -1, "ingroup2");
    childCmd->Flags()->StringSlice("ingroup3", {}, "ingroup3");
    childCmd->Flags()->Bool("nogroup", false, "nogroup");
    childCmd->MarkFlagsMutuallyExclusive("ingroup1", "ingroup2", "ingroup3");
    auto noDesc = string::fromStatic(cmd::ShellCompNoDescRequestCmd);
    std::string_view nofile = "Completion ended with directive: ShellCompDirectiveNoFileComp";

    auto [output, err] = executeCommand(*rootCmd, {noDesc, "child", "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--ingroup1", "--only-if-all", "--help", "-h", "--ingroup2", "--ingroup3", "--nogroup", ":4", nofile}));

    // Flags in the group are hidden once one of them is given, except it
    // (a slice).
    std::tie(output, err) = executeCommand(*rootCmd, {noDesc, "child", "--ingroup3", "a", "-"});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"--only-if-all", "--help", "-h", "--ingroup3", "--nogroup", ":4", nofile}));
}

TEST(GoCmd, ActiveHelpAlone) {
    auto rootCmd = cmd::New({.Use = "root", .Run = emptyRun});
    std::string activeHelpFunc = "This is active help";
    rootCmd->ValidArgsFunction = [&](Command&, std::span<const string>, std::string_view) {
        return std::pair{cmd::AppendActiveHelp({}, activeHelpFunc), cmd::ShellCompDirectiveDefault};
    };
    auto [output, err] = executeCommand(*rootCmd, {string::fromStatic(cmd::ShellCompRequestCmd), ""});
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(output, lines({"_activeHelp_ " + activeHelpFunc, ":0", "Completion ended with directive: ShellCompDirectiveDefault"}));
}

TEST(GoCmd, GetEnvConfig) {
    EXPECT_EQ(cmd::detail::configEnvVar("root", "COMPLETION_DESCRIPTIONS"), "ROOT_COMPLETION_DESCRIPTIONS");
    EXPECT_EQ(cmd::detail::configEnvVar("my-app:v1", "ACTIVE_HELP"), "MY_APP_V1_ACTIVE_HELP");
}

TEST(GoCmd, CompletionScripts) {
    auto rootCmd = cmd::New({.Use = "my-app", .Run = emptyRun});
    auto gen = [&](auto f) {
        auto buf = make_ptr<bytes::Buffer>();
        EXPECT_FALSE(f(*buf));
        return buf->String().str();
    };
    std::string bash = gen([&](io::Writer& w) { return rootCmd->GenBashCompletionV2(w, true); });
    checkStringContains(bash, "# bash completion V2 for my-app                               -*- shell-script -*-");
    checkStringContains(bash, "requestComp=\"${words[0]} __complete ${args[*]}\"");
    checkStringContains(bash, "complete -o default -F __start_my-app my-app");
    std::string bashNoDesc = gen([&](io::Writer& w) { return rootCmd->GenBashCompletionV2(w, false); });
    checkStringContains(bashNoDesc, "__completeNoDesc");
    std::string zsh = gen([&](io::Writer& w) { return rootCmd->GenZshCompletion(w); });
    checkStringContains(zsh, "#compdef my-app");
    checkStringContains(zsh, "compdef _my-app my-app");
    std::string fish = gen([&](io::Writer& w) { return rootCmd->GenFishCompletion(w, true); });
    checkStringContains(fish, "# fish completion for my-app                               -*- shell-script -*-");
    checkStringContains(fish, "function __my_app_perform_completion");
    checkStringContains(fish, "MY_APP_ACTIVE_HELP");
    std::string ps = gen([&](io::Writer& w) { return rootCmd->GenPowerShellCompletionWithDesc(w); });
    checkStringContains(ps, "Register-ArgumentCompleter -CommandName 'my-app' -ScriptBlock ${__my_appCompleterBlock}");
    checkStringContains(ps, "$_ -replace '\\s|#|@|\\$|;|,|''|\\{|\\}|\\(|\\)|\"|`|\\||<|>|&','`$&'");
}
