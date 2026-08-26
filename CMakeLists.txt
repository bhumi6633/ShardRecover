cmake_minimum_required(VERSION 3.20)

project(ShardRecover LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(shardrecover cli/main.cpp)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shardrecover PRIVATE -Wall -Wextra -Wpedantic)
endif()
