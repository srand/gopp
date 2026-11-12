#include <gopp/os.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace gopp::os {

namespace {

std::vector<string>& argv_storage() {
    static std::vector<string>* v = new std::vector<string>();
    return *v;
}

}  // namespace

const std::vector<string>& Args() { return argv_storage(); }

void SetArgs(int argc, char** argv) {
    auto& v = argv_storage();
    v.clear();
    for (int i = 0; i < argc; i++) v.emplace_back(argv[i]);
}

string Getenv(std::string_view key) {
    const char* v = std::getenv(std::string(key).c_str());
    return v ? string(v) : string();
}

void Exit(int code) {
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace gopp::os
