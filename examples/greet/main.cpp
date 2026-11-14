// greet: the smallest cmd program, a command with a subcommand, flags
// and arguments.
//
//     $ greet hello Gopher                 # Hello, Gopher!
//     $ greet hello -n 3 --shout world     # HELLO, WORLD! three times
//     $ greet hello                        # Error: requires at least 1 arg(s)...
//     $ greet --help                       # the commands and flags
//     $ greet help hello                   # a command's help
//
// cmd is go++'s port of cobra: the help, the help command, --help, error
// messages and shell completion (`greet completion bash`) come with it.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

using namespace gopp;

int main() {
    isize times = 1;
    bool shout = false;

    auto root = cmd::New({
        .Use = "greet",
        .Short = "greet says hello",
    });

    auto hello = cmd::New({
        .Use = "hello NAME...",
        .Short = "Say hello to someone",
        .Example = "  greet hello Gopher\n  greet hello -n 3 --shout world",
        .Args = cmd::MinimumNArgs(1),
        .Run = [&](cmd::Command& c, std::span<const string> args) {
            string greeting = "Hello, "_s + strings::Join(args, " and ") + "!";
            if (shout) greeting = strings::ToUpper(greeting);
            for (isize i = 0; i < times; i++) fmt::Println(c.OutOrStdout(), "{}", greeting);
        },
    });
    hello->Flags()->IntVarP(&times, "times", "n", 1, "how many times to say it");
    hello->Flags()->BoolVar(&shout, "shout", false, "say it loudly");

    root->AddCommand(hello);
    return root->Execute() ? 1 : 0;
}
