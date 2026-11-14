// The hash package: the interface of hash functions, such as sha256::New's.
//
//     auto h = sha256::New();
//     io::WriteString(*h, "hello\n");
//     bytes sum = h->Sum(bytes());   // and h goes on hashing
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/io.hpp>
#include <gopp/slice.hpp>
#endif

namespace gopp::hash {

// hash.Hash: a Writer that never returns an error, of which the hash of all
// that was written so far can be had at any time.
struct Hash : io::Writer {
    // Appends the hash of what was written to b and returns the result. It
    // doesn't change the state: writing can go on.
    virtual slice<byte> Sum(slice<byte> b) const = 0;
    // Back to the state of a new hash.
    virtual void Reset() = 0;
    // The number of bytes Sum appends.
    virtual isize Size() const = 0;
    // The block size; Write is fastest with multiples of it.
    virtual isize BlockSize() const = 0;
};

}  // namespace gopp::hash
