// sha256sum: prints the checksums of files in the format of coreutils'
// sha256sum, hashing several files at once. Each file gets a fiber, which
// streams it through a hash::Hash with io::Copy; a channel per file hands
// the results back in the order of the arguments.
//
//     $ sha256sum *.cpp > SUMS
//     $ sha256sum -c SUMS                   # checks them; so does coreutils' sha256sum -c
//     $ sha256sum -a md5 -j 32 $(find src -type f)
//     $ echo hello | sha256sum              # standard input, as "-"
//
// -a picks the hash: md5, sha1, sha224, sha256 (the default), sha384 or
// sha512. -j is the number of files hashed at once.
#include <gopp/chan.hpp>
#include <gopp/crypto/md5.hpp>
#include <gopp/crypto/sha1.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/crypto/sha512.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/hash.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <map>
#include <vector>

using namespace gopp;

using newHash = ptr<hash::Hash> (*)();

const std::map<std::string_view, newHash> algorithms = {
    {"md5", md5::New},       {"sha1", sha1::New},      {"sha224", sha256::New224},
    {"sha256", sha256::New}, {"sha384", sha512::New384}, {"sha512", sha512::New},
};

struct Result {
    string sum;  // in hex
    error err;
};

// The checksum of a file, or of standard input for "-".
Result sumFile(newHash algorithm, const string& name) {
    ptr<hash::Hash> h = algorithm();
    if (name == "-") {
        if (auto [_, err] = io::Copy(*h, os::Stdin); err) return {{}, err};
    } else {
        auto [f, err] = os::Open(name);
        if (err) return {{}, err};
        auto [_, copyErr] = io::Copy(*h, *f);
        f->Close();
        if (copyErr) return {{}, copyErr};
    }
    return {hex::EncodeToString(h->Sum(slice<byte>())), nullptr};
}

// Hashes the files, at most jobs at once. The i-th channel gets the i-th
// file's result.
std::vector<chan<Result>> sumAll(newHash algorithm, const std::vector<string>& names, isize jobs) {
    std::vector<chan<Result>> results;
    for (std::size_t i = 0; i < names.size(); i++) results.push_back(make_chan<Result>(1));
    // A buffered channel as a semaphore: a send takes a slot, a receive
    // gives it back.
    auto slots = make_chan<empty>(jobs);
    go([algorithm, names, results, slots] {
        for (std::size_t i = 0; i < names.size(); i++) {
            slots.send({});
            go([algorithm, name = names[i], result = results[i], slots] {
                result.send(sumFile(algorithm, name));
                slots.recv();
            });
        }
    });
    return results;
}

int main(int argc, char** argv) {
    string algorithm = "sha256";
    bool check = false;
    isize jobs = 8;
    flag::StringVar(&algorithm, "a", algorithm, "the hash: md5, sha1, sha224, sha256, sha384 or sha512");
    flag::BoolVar(&check, "c", false, "read checksums from the files and check them");
    flag::IntVar(&jobs, "j", jobs, "the files hashed at once");
    flag::Parse(argc, argv);
    auto found = algorithms.find(algorithm.view());
    if (found == algorithms.end() || jobs < 1) {
        fmt::Println(os::Stderr, "usage: sha256sum [-a hash] [-c] [-j n] [file ...]");
        return 2;
    }
    newHash h = found->second;
    std::vector<string> args = flag::Args();
    if (args.empty()) args.push_back("-");

    if (!check) {
        std::vector<chan<Result>> results = sumAll(h, args, jobs);
        int status = 0;
        for (std::size_t i = 0; i < args.size(); i++) {
            Result r = results[i].recv();
            if (r.err) {
                fmt::Println(os::Stderr, "sha256sum: {}", r.err);
                status = 1;
                continue;
            }
            fmt::Println("{}  {}", r.sum, args[i]);
        }
        return status;
    }

    // -c: the arguments are lists of "checksum  name" lines.
    std::vector<string> names, want;
    for (const string& list : args) {
        auto [data, err] = list == "-" ? io::ReadAll(os::Stdin) : os::ReadFile(list);
        if (err) {
            fmt::Println(os::Stderr, "sha256sum: {}", err);
            return 1;
        }
        for (string line : strings::Lines(String(data))) {
            line = strings::TrimRight(line, "\r\n");
            // Two spaces, or a space and a '*' for coreutils' binary mode.
            auto [sum, name, ok] = strings::Cut(line, " ");
            if (!ok || name.empty() || (name[0] != ' ' && name[0] != '*')) {
                if (!strings::TrimSpace(line).empty()) {
                    fmt::Println(os::Stderr, "sha256sum: {}: not a checksum line: {}", list, line);
                }
                continue;
            }
            want.push_back(sum);
            names.push_back(name.slice(1));
        }
    }
    std::vector<chan<Result>> results = sumAll(h, names, jobs);
    int failed = 0, unreadable = 0;
    for (std::size_t i = 0; i < names.size(); i++) {
        Result r = results[i].recv();
        if (r.err) {
            fmt::Println(os::Stderr, "sha256sum: {}", r.err);
            fmt::Println("{}: FAILED open or read", names[i]);
            unreadable++;
        } else if (!strings::EqualFold(r.sum, want[i])) {
            fmt::Println("{}: FAILED", names[i]);
            failed++;
        } else {
            fmt::Println("{}: OK", names[i]);
        }
    }
    if (unreadable > 0) fmt::Println(os::Stderr, "sha256sum: WARNING: {} listed files could not be read", unreadable);
    if (failed > 0) fmt::Println(os::Stderr, "sha256sum: WARNING: {} computed checksums did NOT match", failed);
    return failed + unreadable > 0 ? 1 : 0;
}
