# Asserts the package installed with GRAPHLINT_WITH_CUDA=OFF: the reader is not part of it,
# neither the header nor a static library. A leak here only shows on a machine without a
# toolkit, and the host-only job is the one machine that has none.
#
# Usage: cmake -DPREFIX=<install prefix> -P check_host_package.cmake

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "check_host_package.cmake: PREFIX is required")
endif()

set(header "${PREFIX}/include/graphlint/cuda_source.hpp")
if(EXISTS "${header}")
    message(FATAL_ERROR "the host-only package carries the reader header: ${header}")
endif()

# lib or lib64, .lib or .a: the layout is the platform's, the name is ours.
file(GLOB cuda_libraries "${PREFIX}/lib*/graphlint_cuda.*")
if(cuda_libraries)
    message(FATAL_ERROR "the host-only package carries the reader library: ${cuda_libraries}")
endif()

message(STATUS "host-only package: no reader header, no reader library")
