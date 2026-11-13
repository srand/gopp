// Helper commands: see go_helper.hpp.
#include "go_helper.hpp"

#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

using namespace gopp;

namespace go_helper {

namespace {

std::map<std::string, helper_fn>& helpers() {
    static auto* m = new std::map<std::string, helper_fn>();
    return *m;
}

// This program's arguments: main hasn't run yet when they're needed.
// The arguments, as glibc passes them to the functions of .init_array:
// this one runs before the static initializers (priority 101), and works
// where /proc isn't mounted (a child in a chroot).
int init_argc = 0;
char** init_argv = nullptr;
[[gnu::constructor(101)]] void capture_args(int argc, char** argv, char**) {
    init_argc = argc;
    init_argv = argv;
}

std::vector<std::string> cmdline() {
    if (init_argv) return std::vector<std::string>(init_argv, init_argv + init_argc);
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < all.size()) {
        std::size_t j = all.find('\0', i);
        if (j == std::string::npos) j = all.size();
        args.push_back(all.substr(i, j - i));
        i = j + 1;
    }
    return args;
}

int saved_stdout = -1;

// Runs the helper before the tests, and exits.
class helper_environment final : public testing::Environment {
public:
    explicit helper_environment(std::vector<std::string> args) : args_(std::move(args)) {}

    void SetUp() override {
        // googletest's output so far went to /dev/null: standard output is
        // the helper's from here on.
        std::fflush(stdout);
        ::dup2(saved_stdout, 1);
        ::close(saved_stdout);
        auto it = helpers().find(args_[1]);
        if (it == helpers().end()) {
            std::fprintf(stderr, "Unknown command \"%s\"\n", args_[1].c_str());
            os::Exit(2);
        }
        it->second(std::vector<std::string>(args_.begin() + 2, args_.end()));
        os::Exit(0);
    }

private:
    std::vector<std::string> args_;
};

// A child started by helper_command: GO_EXEC_TEST_PID is set and the first
// argument is a command, not a flag (googletest's, as other tests' children
// get).
const bool helper_mode = [] {
    if (!std::getenv("GO_EXEC_TEST_PID")) return false;
    std::vector<std::string> args = cmdline();
    if (args.size() < 2 || args[1].empty() || args[1][0] == '-') return false;
    saved_stdout = ::fcntl(1, F_DUPFD_CLOEXEC, 100);
    int dn = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    // Without /dev/null (in a chroot), a file in memory.
    if (dn < 0) dn = ::memfd_create("null", MFD_CLOEXEC);
    ::dup2(dn, 1);
    ::close(dn);
    testing::AddGlobalTestEnvironment(new helper_environment(std::move(args)));
    return true;
}();

}  // namespace

bool register_helper(const std::string& name, helper_fn f) {
    if (helpers().count(name)) {
        std::fprintf(stderr, "duplicate command registered: %s\n", name.c_str());
        std::abort();
    }
    helpers()[name] = std::move(f);
    return true;
}

string executable() {
    auto [exe, err] = os::Executable();
    if (err) ADD_FAILURE() << "Executable: " << err;
    return exe;
}

ptr<os::exec::Cmd> helper_command_context(ptr<context::Context> ctx, const std::string& name,
                                          const std::vector<std::string>& args) {
    // Go's TestMain sets GO_EXEC_TEST_PID for the children.
    static std::once_flag once;
    std::call_once(once, [] { os::Setenv("GO_EXEC_TEST_PID", std::to_string(os::Getpid())); });
    std::vector<string> cs{string(name)};
    for (auto& a : args) cs.emplace_back(a);
    if (ctx) return os::exec::CommandContext(ctx, executable(), std::span<const string>(cs));
    return os::exec::Command(executable(), std::span<const string>(cs));
}

}  // namespace go_helper
