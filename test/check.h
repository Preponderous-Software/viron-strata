// A minimal test harness: TEST(name, tag) registers a test; CHECK records a
// failure and carries on; REQUIRE stops the test. test_main.cpp runs the
// tests whose tag matches its argument ("[unit]", "[render]"), or all.
#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace check {

struct Test {
    std::string name, tag;
    std::function<void()> fn;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, const char* tag, std::function<void()> fn) {
        registry().push_back({name, tag, std::move(fn)});
    }
};

struct Abort : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void fail(const char* file, int line, const std::string& what);

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name, tag)                                                                     \
    static void CHECK_CAT(test_fn_, __LINE__)();                                            \
    static check::Register CHECK_CAT(test_reg_, __LINE__)(name, tag, CHECK_CAT(test_fn_, __LINE__)); \
    static void CHECK_CAT(test_fn_, __LINE__)()
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) check::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)
#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) {                                                          \
            check::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");              \
            throw check::Abort("REQUIRE failed");                               \
        }                                                                       \
    } while (0)
