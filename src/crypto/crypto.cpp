// crypto: hash identifiers over go++'s hashes.
#include <gopp/crypto.hpp>
#include <gopp/crypto/md5.hpp>
#include <gopp/crypto/sha1.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/crypto/sha512.hpp>

#include <string>

namespace gopp::crypto {

bool Available(Hash h) {
    switch (h) {
        case Hash::MD5:
        case Hash::SHA1:
        case Hash::SHA224:
        case Hash::SHA256:
        case Hash::SHA384:
        case Hash::SHA512:
            return true;
    }
    return false;
}

ptr<hash::Hash> New(Hash h) {
    switch (h) {
        case Hash::MD5: return md5::New();
        case Hash::SHA1: return sha1::New();
        case Hash::SHA224: return sha256::New224();
        case Hash::SHA256: return sha256::New();
        case Hash::SHA384: return sha512::New384();
        case Hash::SHA512: return sha512::New();
    }
    panic("crypto: requested hash function is unavailable");
}

isize Size(Hash h) {
    switch (h) {
        case Hash::MD5: return md5::Size;
        case Hash::SHA1: return sha1::Size;
        case Hash::SHA224: return sha256::Size224;
        case Hash::SHA256: return sha256::Size;
        case Hash::SHA384: return sha512::Size384;
        case Hash::SHA512: return sha512::Size;
    }
    panic("crypto: Size of unknown hash function");
}

string String(Hash h) {
    switch (h) {
        case Hash::MD5: return "MD5";
        case Hash::SHA1: return "SHA-1";
        case Hash::SHA224: return "SHA-224";
        case Hash::SHA256: return "SHA-256";
        case Hash::SHA384: return "SHA-384";
        case Hash::SHA512: return "SHA-512";
    }
    return "unknown hash value " + std::to_string(static_cast<unsigned>(h));
}

}  // namespace gopp::crypto
