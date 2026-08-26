cmake_minimum_required(VERSION 3.20)

project(ShardRecover LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shardrecover_core src/binary_file.cpp)
target_include_directories(shardrecover_core PUBLIC include)

add_executable(shardrecover cli/main.cpp)
target_link_libraries(shardrecover PRIVATE shardrecover_core)

if(BUILD_TESTING)
    add_executable(binary_file_tests tests/binary_file_tests.cpp)
    target_link_libraries(binary_file_tests PRIVATE shardrecover_core)
    add_test(NAME binary_file_tests COMMAND binary_file_tests)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shardrecover_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shardrecover PRIVATE -Wall -Wextra -Wpedantic)
    if(BUILD_TESTING)
        target_compile_options(binary_file_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
