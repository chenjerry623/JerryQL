#pragma once

// A minimal test harness, so the project has no third-party dependencies.
//
//   TEST(addsNumbers) { CHECK_EQ(1 + 1, 2); }

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    std::cerr << file << ":" << line << ": FAILED: " << message << "\n";
    ++failureCount();
}

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

}  // namespace check

#define TEST(name)                                                   \
    static void name();                                              \
    static check::Registrar registrar_##name(#name, name);           \
    static void name()

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) check::reportFailure(__FILE__, __LINE__, #cond); \
    } while (0)

// Copies both sides, because (actual) may refer into a temporary.
#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const auto a_ = (actual);                                                     \
        const auto e_ = (expected);                                                   \
        if (!(a_ == e_)) {                                                            \
            check::reportFailure(__FILE__, __LINE__,                                  \
                                 std::string(#actual) + "\n  actual:   " +            \
                                     check::show(a_) + "\n  expected: " + check::show(e_)); \
        }                                                                             \
    } while (0)

// Passes if `statement` throws `exception_type` and the message contains `fragment`.
#define CHECK_THROWS(statement, exception_type, fragment)                              \
    do {                                                                               \
        bool thrown_ = false;                                                          \
        try {                                                                          \
            statement;                                                                 \
        } catch (const exception_type& ex_) {                                          \
            thrown_ = true;                                                            \
            if (std::string(ex_.what()).find(fragment) == std::string::npos) {         \
                check::reportFailure(__FILE__, __LINE__,                               \
                                     std::string("message '") + ex_.what() +           \
                                         "' lacks '" + (fragment) + "'");              \
            }                                                                          \
        }                                                                              \
        if (!thrown_) check::reportFailure(__FILE__, __LINE__, #statement " did not throw"); \
    } while (0)
