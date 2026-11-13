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
// of a key (compared ignoring case if case_insensitive); with !nul_ok,
// drops the variables with a NUL and returns an error for them.
std::pair<slice<string>, error> dedup_env_case(bool case_insensitive, bool nul_ok, const slice<string>& env);

// Go's filepath.Clean.
std::string clean_path(std::string_view path);

}  // namespace gopp::os::exec::detail
