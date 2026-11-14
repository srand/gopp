// Internals of os/exec, shared with the tests.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace gopp::os::exec::detail {

// Go's dedupEnvCase: env without duplicate keys, keeping the last value
// of a key (compared ignoring case if caseInsensitive); with !nulOk,
// drops the variables with a NUL and returns an error for them.
std::pair<slice<string>, error> dedupEnvCase(bool caseInsensitive, bool nulOk, const slice<string>& env);

// Go's filepath.Clean.
std::string cleanPath(std::string_view path);

}  // namespace gopp::os::exec::detail
