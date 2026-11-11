// With GOPP_DEBUG_LEAKS (jolt build test:leaks=true,repeat=2): fails each
// test that ends with more live gopp::objects than it started with. Fibers a
// test started may still be finishing when it returns, so a test gets a
// moment for the count to settle. With repeat=2 only the second round
// counts, so that objects created once and kept for good (default clients,
// caches) aren't taken for leaks.
#include <gopp/ptr.hpp>

#include <gtest/gtest.h>

#ifdef GOPP_DEBUG_LEAKS

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

class leak_listener final : public testing::EmptyTestEventListener {
public:
    void OnTestIterationStart(const testing::UnitTest&, int iteration) override {
        checking_ = GTEST_FLAG_GET(repeat) == 1 || iteration > 0;
    }

    void OnTestStart(const testing::TestInfo&) override {
        before_ = gopp::objects_in_use();
        created_ = gopp::objects_created();
    }

    void OnTestEnd(const testing::TestInfo& t) override {
        if (!checking_) return;
        long now = gopp::objects_in_use();
        for (int i = 0; i < 300 && now > before_; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            now = gopp::objects_in_use();
        }
        if (now > before_) {
            std::string name = std::string(t.test_suite_name()) + "." + t.name();
            std::fprintf(stderr, "LEAK: %s leaked %ld objects:\n", name.c_str(), now - before_);
            for (auto& t : gopp::live_objects_since(created_)) std::fprintf(stderr, "    %s\n", t.c_str());
            leaky_.push_back(name + " (" + std::to_string(now - before_) + ")");
        }
    }

    void OnTestProgramEnd(const testing::UnitTest&) override {
        if (leaky_.empty()) {
            std::fprintf(stderr, "leak check: no test leaked objects\n");
            return;
        }
        std::fprintf(stderr, "leak check: %zu tests leaked objects:\n", leaky_.size());
        for (auto& l : leaky_) std::fprintf(stderr, "  %s\n", l.c_str());
        std::fflush(nullptr);
        std::_Exit(1);
    }

private:
    bool checking_ = true;
    long before_ = 0;
    unsigned long created_ = 0;
    std::vector<std::string> leaky_;
};

const bool registered = [] {
    testing::UnitTest::GetInstance()->listeners().Append(new leak_listener);
    return true;
}();

}  // namespace

#endif
