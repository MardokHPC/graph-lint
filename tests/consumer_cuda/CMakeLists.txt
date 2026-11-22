cmake_minimum_required(VERSION 3.20)

project(graphlint_consumer_cuda LANGUAGES CXX)

find_package(graphlint CONFIG REQUIRED)

add_executable(consumer_cuda main.cpp other.cpp)
target_link_libraries(consumer_cuda PRIVATE graphlint::cuda)

enable_testing()
add_test(NAME consumer_cuda COMMAND consumer_cuda)
