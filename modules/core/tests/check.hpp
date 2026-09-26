#pragma once

// Minimal assertion harness.
//
// Mira's core carries no third-party dependency, and that includes its own
// test build: pulling doctest/Catch2 in just to print "1 assertion passed"
// would make the zero-dependency claim untrue for anyone vendoring the repo.
// Roughly forty lines buy pass/fail counting, file:line reporting, and a
// non-zero exit code, which is all CI reads.

#include <cstdio>
#include <string_view>

namespace Mira::test {

inline int failures = 0;
inline int checks = 0;

inline void report(bool ok, std::string_view expression, std::string_view file, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr,
                     "FAIL %.*s:%d  %.*s\n",
                     static_cast<int>(file.size()),
                     file.data(),
                     line,
                     static_cast<int>(expression.size()),
                     expression.data());
    }
}

inline void section(std::string_view name) {
    std::fprintf(stdout, "── %.*s\n", static_cast<int>(name.size()), name.data());
}

[[nodiscard]] inline int summary() {
    std::fprintf(stdout, "\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace Mira::test

#define CHECK(expr) ::Mira::test::report((expr), #expr, __FILE__, __LINE__)

#define CHECK_THROWS(expr, exception_type)                                                         \
    do {                                                                                           \
        bool caught = false;                                                                       \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const exception_type&) {                                                          \
            caught = true;                                                                         \
        } catch (...) {                                                                            \
        }                                                                                          \
        ::Mira::test::report(caught, #expr " throws " #exception_type, __FILE__, __LINE__);    \
    } while (false)
