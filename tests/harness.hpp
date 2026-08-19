#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace harness {

inline int checks = 0;
inline int failures = 0;
inline const char* current = "";

struct Test {
    const char* name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& expr) {
    ++failures;
    std::printf("  FAIL %s:%d  %s\n", file, line, expr.c_str());
}

inline int run() {
    for (const auto& test : registry()) {
        current = test.name;
        const int before = failures;
        test.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", test.name);
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace harness

#define TEST(name)                                    \
    static void name();                               \
    static harness::Register harness_reg_##name(#name, name); \
    static void name()

#define CHECK(expr)                                       \
    do {                                                  \
        ++harness::checks;                                \
        if (!(expr)) harness::fail(__FILE__, __LINE__, #expr); \
    } while (0)

#define CHECK_EQ(a, b)                                                            \
    do {                                                                          \
        ++harness::checks;                                                        \
        const auto lhs_ = (a);                                                    \
        const auto rhs_ = (b);                                                    \
        if (!(lhs_ == rhs_))                                                      \
            harness::fail(__FILE__, __LINE__,                                     \
                          std::string(#a " == " #b " (") + std::to_string(lhs_) + \
                              " vs " + std::to_string(rhs_) + ")");               \
    } while (0)

#define TEST_MAIN int main() { return harness::run(); }
