// Main for the host-only suite; M2 device suites get their own main and skip with 77.
#include <cstdio>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // A case skips when its artifact is absent. ctest maps 77 to SKIPPED, so a binary
    // whose every case skipped is not reported green: same rule as the device main.
    const ::testing::UnitTest* unit = ::testing::UnitTest::GetInstance();
    if (result == 0 && unit->skipped_test_count() > 0 &&
        unit->skipped_test_count() == unit->test_to_run_count()) {
        std::printf("graphlint: every case was skipped, exiting 77\n");
        return 77;
    }
    return result;
}
