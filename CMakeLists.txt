cmake_minimum_required(VERSION 3.20)
project(matchbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(matchbook src/order_book.cpp)
target_include_directories(matchbook PUBLIC include)
target_compile_options(matchbook PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE matchbook)
add_test(NAME order_book COMMAND test_order_book)

add_executable(test_differential tests/test_differential.cpp)
target_link_libraries(test_differential PRIVATE matchbook)
add_test(NAME differential COMMAND test_differential)

add_executable(bench_order_book bench/bench_order_book.cpp)
target_link_libraries(bench_order_book PRIVATE matchbook)
target_compile_options(bench_order_book PRIVATE -O3)
