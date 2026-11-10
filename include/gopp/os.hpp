// The os package (standard streams, arguments, exit, environment).
//
// os::Stdout and os::Stderr assume a C library whose stdout and stderr
// macros expand to themselves, as glibc's do.
#pragma once

#include <gopp/io.hpp>
#include <gopp/string.hpp>

#include <string_view>
#include <vector>

namespace gopp::os {

// os.Stdout, os.Stderr: writers on file descriptors 1 and 2.
extern io::Writer& Stdout;
extern io::Writer& Stderr;

// os.Args, as given to os::SetArgs (flag::Parse calls it).
const std::vector<string>& Args();
void SetArgs(int argc, char** argv);

// os.Getenv
string Getenv(std::string_view key);

// os.Exit: exits at once, without running destructors.
[[noreturn]] void Exit(int code);

}  // namespace gopp::os
