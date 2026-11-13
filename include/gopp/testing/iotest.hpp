// The testing/iotest package: checks of Readers.
//
//     if (auto err = iotest::TestReader(*f, content)) ADD_FAILURE() << err;
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>

#include <span>
#endif

namespace gopp::iotest {

// iotest.TestReader: tests that reading from r returns the expected file
// content. It reads in small pieces, checks EOF, and where r is also an
// io::Seeker or io::ReaderAt, checks Seek and ReadAt; it returns the first
// misbehavior it finds.
error TestReader(io::Reader& r, std::span<const byte> content);

}  // namespace gopp::iotest
