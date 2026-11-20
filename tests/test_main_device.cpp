// Main for the suites that open a CUDA handle. ctest maps exit code 77 to
// SKIPPED, so the same binary is green on the GPU box and "not here" elsewhere.
#include <cuda_runtime_api.h>

#include <cstdio>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int devices = 0;
    const cudaError_t status = cudaGetDeviceCount(&devices);
    if (status != cudaSuccess || devices == 0) {
        std::printf("graphlint: no CUDA device (%s), exiting 77\n",
                    status == cudaSuccess ? "0 devices" : cudaGetErrorString(status));
        cudaGetLastError();  // the failed query stays pending and would be the first error seen
        return 77;
    }
    return RUN_ALL_TESTS();
}
