#pragma once

#define GRAPHLINT_VERSION_MAJOR 0
#define GRAPHLINT_VERSION_MINOR 3
#define GRAPHLINT_VERSION_PATCH 0
#define GRAPHLINT_VERSION_STRING "0.3.0"

#include <cstdio>
#include <cstdlib>

// Explicit 0/1 wins over NDEBUG, so a Release build can be asked to keep the checks.
#ifndef GRAPHLINT_ENABLE_ASSERTS
#  ifdef NDEBUG
#    define GRAPHLINT_ENABLE_ASSERTS 0
#  else
#    define GRAPHLINT_ENABLE_ASSERTS 1
#  endif
#endif

namespace graphlint {
namespace detail {

[[noreturn]] inline void assert_fail(const char* expression, const char* file, int line) {
    // stderr is a macro mandated by <cstdio>, not a std name: std::stderr is std::(&...) on MSVC.
    std::fprintf(stderr, "graphlint: assertion failed: %s (%s:%d)\n", expression, file, line);
    std::abort();
}

}  // namespace detail
}  // namespace graphlint

// Variadic: a condition such as std::is_same_v<A, B> contains a comma.
#if GRAPHLINT_ENABLE_ASSERTS
#define GRAPHLINT_ASSERT(...)                                                          \
    ((void)((__VA_ARGS__)                                                              \
                ? (void)0                                                              \
                : ::graphlint::detail::assert_fail(#__VA_ARGS__, __FILE__, __LINE__)))
#else
#define GRAPHLINT_ASSERT(...) ((void)0)
#endif
