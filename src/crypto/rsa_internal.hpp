// Internal to go++'s crypto/rsa: for its tests.
#pragma once

#include <gopp/crypto/rsa.hpp>

#include <string_view>

namespace gopp::rsa::detail {

// While one is alive, keys under 1024 bits are accepted by the signing and
// verifying functions: Go's GODEBUG=rsa1024min=0, for Go's 512-bit test keys.
struct AllowSmallKeys {
    AllowSmallKeys();
    ~AllowSmallKeys();
    AllowSmallKeys(const AllowSmallKeys&) = delete;
    AllowSmallKeys& operator=(const AllowSmallKeys&) = delete;
};

// A private key of two primes from its parts, given in hexadecimal. Go's
// tests parse their keys from PEM, which needs crypto/x509.
ptr<PrivateKey> fromComponents(std::string_view n, int e, std::string_view d, std::string_view p,
                               std::string_view q);

}  // namespace gopp::rsa::detail
