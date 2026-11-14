// Ported from Go 1.27.3's os/env_test.go, with Go's test names. See
// go_os_test.cpp.
#include "go_os_test_util.hpp"

using namespace os_test;

namespace {

// testGetenv gives us a controlled set of variables for testing Expand.
string testGetenv(std::string_view s) {
    if (s == "*") return "all the args";
    if (s == "#") return "NARGS";
    if (s == "$") return "PID";
    if (s == "1") return "ARGUMENT1";
    if (s == "HOME") return "/usr/gopher";
    if (s == "H") return "(Value of H)";
    if (s == "home_1") return "/usr/foo";
    if (s == "_") return "underscore";
    return "";
}

struct expandTest {
    std::string in, out;
};

const std::vector<expandTest> expandTests = {
    {"", ""},
    {"$*", "all the args"},
    {"$$", "PID"},
    {"${*}", "all the args"},
    {"$1", "ARGUMENT1"},
    {"${1}", "ARGUMENT1"},
    {"now is the time", "now is the time"},
    {"$HOME", "/usr/gopher"},
    {"$home_1", "/usr/foo"},
    {"${HOME}", "/usr/gopher"},
    {"${H}OME", "(Value of H)OME"},
    {"A$$$#$1$H$home_1*B", "APIDNARGSARGUMENT1(Value of H)/usr/foo*B"},
    {"start$+middle$^end$", "start$+middle$^end$"},
    {"mixed$|bag$$$", "mixed$|bagPID$"},
    {"$", "$"},
    {"$}", "$}"},
    {"${", ""},   // invalid syntax; eat up the characters
    {"${}", ""},  // invalid syntax; eat up the characters
};

}  // namespace

TEST(GoOs, Expand) {
    for (const auto& test : expandTests) {
        string result = os::Expand(test.in, testGetenv);
        EXPECT_EQ(result, test.out) << "Expand(" << test.in << ")";
    }
}

TEST(GoOs, ConsistentEnviron) {
    slice<string> e0 = os::Environ();
    for (int i = 0; i < 10; i++) {
        slice<string> e1 = os::Environ();
        ASSERT_TRUE(std::equal(e0.begin(), e0.end(), e1.begin(), e1.end())) << "environment changed";
    }
}

TEST(GoOs, Unsetenv) {
    const std::string testKey = "GO_TEST_UNSETENV";
    auto set = [&] {
        std::string prefix = testKey + "=";
        for (const string& key : os::Environ()) {
            if (key.starts_with(prefix)) return true;
        }
        return false;
    };
    ASSERT_FALSE(os::Setenv(testKey, "1")) << "Setenv";
    EXPECT_TRUE(set()) << "Setenv didn't set TestUnsetenv";
    ASSERT_FALSE(os::Unsetenv(testKey)) << "Unsetenv";
    EXPECT_FALSE(set()) << "Unsetenv didn't clear TestUnsetenv";
}

TEST(GoOs, Clearenv) {
    const std::string testKey = "GO_TEST_CLEARENV";
    const std::string testValue = "1";

    // Reset the environment at the end.
    struct restore {
        slice<string> orig = os::Environ();
        ~restore() {
            for (const string& pair : orig) {
                std::size_t i = pair.view().substr(1).find('=') + 1;
                if (error err = os::Setenv(pair.view().substr(0, i), pair.view().substr(i + 1))) {
                    ADD_FAILURE() << "Setenv(" << pair << ") failed during reset: " << err;
                }
            }
        }
    } restore;

    ASSERT_FALSE(os::Setenv(testKey, testValue));
    EXPECT_TRUE(os::LookupEnv(testKey).second) << "Setenv didn't set $" << testKey;
    os::Clearenv();
    auto [val, ok] = os::LookupEnv(testKey);
    EXPECT_FALSE(ok) << "Clearenv() didn't clear $" << testKey << ", remained with value " << val;
}

TEST(GoOs, LookupEnv) {
    const std::string smallpox = "SMALLPOX";  // No one has smallpox.
    auto [value, ok] = os::LookupEnv(smallpox);  // Should not exist.
    ASSERT_TRUE(!ok && value.empty()) << smallpox << "=" << value;
    ASSERT_FALSE(os::Setenv(smallpox, "virus")) << "failed to release smallpox virus";
    EXPECT_TRUE(os::LookupEnv(smallpox).second) << "smallpox release failed; world remains safe but LookupEnv is broken";
    os::Unsetenv(smallpox);
}

TEST(GoOs, EnvironConsistency) {
    for (const string& kv : os::Environ()) {
        std::size_t i = kv.find('=');
        if (i == 0) i = kv.view().substr(1).find('=') + 1;
        ASSERT_NE(i, string::npos) << "Environ entry missing '=': " << kv;
        std::string_view k = kv.view().substr(0, i);
        std::string_view v = kv.view().substr(i + 1);
        auto [v2, ok] = os::LookupEnv(k);
        EXPECT_TRUE(ok && v == v2) << "Environ contains " << kv << ", but LookupEnv(" << k << ") = " << v2 << ", " << ok;
        // Since k=v is already present in the environment, setting it
        // should be a no-op.
        EXPECT_FALSE(os::Setenv(k, v)) << "Environ contains " << kv << ", but SetEnv failed";
    }
}

// go++'s own: Setenv's errors, Go's syscall.Setenv checks.
TEST(GoOs, SetenvInvalid) {
    for (std::string_view key : {std::string_view(""), std::string_view("A=B"), std::string_view("A\0B", 3)}) {
        error err = os::Setenv(key, "x");
        EXPECT_TRUE(errors::Is(err, syscall::EINVAL_)) << "Setenv(" << key << "): " << err;
    }
    EXPECT_TRUE(errors::Is(os::Setenv("GO_TEST_NUL", std::string_view("a\0b", 3)), syscall::EINVAL_));
    EXPECT_EQ(os::ExpandEnv("${GO_TEST_NO_SUCH_VAR}x"), "x");
}
