// Helper commands: the test program runs itself as another program, as
// Go's os/exec tests do with their TestMain.
//
// helperCommand("echo", "foo") is an exec::Cmd that runs this program as
// `<this program> echo foo`, with GO_EXEC_TEST_PID in its environment.
// Such a child runs the helper registered as "echo" instead of the tests,
// and exits: before main, the child's standard output goes to /dev/null,
// so that googletest's output doesn't mix with the helper's, and a global
// test environment runs the helper before any test.
#pragma once

#include <gopp/context.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>

#include <functional>
#include <string>
#include <vector>

namespace go_helper {

using helperFn = std::function<void(const std::vector<std::string>& args)>;

// Registers a helper command; returns true, for a static initializer.
bool registerHelper(const std::string& name, helperFn f);

// The path of this program.
gopp::string executable();

// A Cmd that runs the helper name with args, with ctx if not nil.
gopp::ptr<gopp::os::exec::Cmd> helperCommandContext(gopp::ptr<gopp::context::Context> ctx, const std::string& name,
                                                      const std::vector<std::string>& args = {});

inline gopp::ptr<gopp::os::exec::Cmd> helperCommand(const std::string& name, const std::vector<std::string>& args = {}) {
    return helperCommandContext(nullptr, name, args);
}

}  // namespace go_helper
