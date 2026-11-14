// The crypto/rand package: cryptographically secure random bytes, from
// getrandom(2).
//
//     std::array<byte, 32> key;
//     rand::Read(key);
//     string token = rand::Text();   // 26 base32 characters, 128 bits
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/io.hpp>
#include <gopp/string.hpp>

#include <span>
#include <utility>
#endif

namespace gopp::rand {

// rand.Reader: a Reader of random bytes. Its Read fills the whole buffer.
extern io::Reader& Reader;

// rand.Read: fills b with random bytes. It never returns an error: if the
// kernel can't supply randomness, the program stops, as in Go.
std::pair<isize, error> Read(std::span<byte> b);

// rand.Text: a random string of 26 characters of the standard base32
// alphabet (A-Z and 2-7): 128 bits of randomness, for tokens and keys.
string Text();

}  // namespace gopp::rand
