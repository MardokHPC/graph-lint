// Both directions of the same fact: the string the header advertises is the
// three integers composed with dots, and it is the CMake project version. A bump
// that touches one place and not the other is the bug this locks out.

#include <graphlint/config.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace {

std::string composed_version() {
    return std::to_string(GRAPHLINT_VERSION_MAJOR) + "." +
           std::to_string(GRAPHLINT_VERSION_MINOR) + "." +
           std::to_string(GRAPHLINT_VERSION_PATCH);
}

}  // namespace

TEST(Config, VersionStringIsTheThreeIntegersComposed) {
    EXPECT_EQ(std::string(GRAPHLINT_VERSION_STRING), composed_version());
}

TEST(Config, HeaderVersionMatchesCmakeProjectVersion) {
    EXPECT_EQ(std::string(GRAPHLINT_VERSION_STRING),
              std::string(GRAPHLINT_TEST_CMAKE_VERSION));
}

// The template argument carries a comma: a single-parameter macro would not
// preprocess. The condition is false, so the death test sees the abort path.
#if GRAPHLINT_ENABLE_ASSERTS
TEST(Config, AssertReportsTheViolatedExpression) {
    EXPECT_DEATH(GRAPHLINT_ASSERT(std::is_same<int, long>::value),
                 "assertion failed: std::is_same");
}
#else
TEST(Config, AssertIsANoOpWhenDisabled) {
    GRAPHLINT_ASSERT(std::is_same<int, long>::value);
}
#endif
