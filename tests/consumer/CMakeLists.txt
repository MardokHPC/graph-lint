cmake_minimum_required(VERSION 3.20)

project(graphlint_consumer LANGUAGES CXX)

find_package(graphlint CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE graphlint::graphlint)

enable_testing()
add_test(NAME consumer COMMAND consumer)
