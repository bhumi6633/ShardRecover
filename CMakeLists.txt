cmake_minimum_required(VERSION 3.20)

project(ShardRecover LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shardrecover_core
    src/binary_file.cpp
    src/fragment_emitter.cpp
    src/fragment_generator.cpp
    src/fragment_graph.cpp
    src/overlap.cpp
    src/reconstruction.cpp
)
target_include_directories(shardrecover_core PUBLIC include)

add_executable(shardrecover
    cli/analyze_command.cpp
    cli/fragment_command.cpp
    cli/main.cpp
)
target_link_libraries(shardrecover PRIVATE shardrecover_core)

if(BUILD_TESTING)
    add_executable(binary_file_tests tests/binary_file_tests.cpp)
    target_link_libraries(binary_file_tests PRIVATE shardrecover_core)
    add_test(NAME binary_file_tests COMMAND binary_file_tests)

    add_executable(fragment_generator_tests tests/fragment_generator_tests.cpp)
    target_link_libraries(fragment_generator_tests PRIVATE shardrecover_core)
    add_test(NAME fragment_generator_tests COMMAND fragment_generator_tests)

    add_executable(fragment_emitter_tests tests/fragment_emitter_tests.cpp)
    target_link_libraries(fragment_emitter_tests PRIVATE shardrecover_core)
    add_test(NAME fragment_emitter_tests COMMAND fragment_emitter_tests)

    add_executable(fragment_graph_tests tests/fragment_graph_tests.cpp)
    target_link_libraries(fragment_graph_tests PRIVATE shardrecover_core)
    add_test(NAME fragment_graph_tests COMMAND fragment_graph_tests)

    add_executable(overlap_tests tests/overlap_tests.cpp)
    target_link_libraries(overlap_tests PRIVATE shardrecover_core)
    add_test(NAME overlap_tests COMMAND overlap_tests)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shardrecover_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shardrecover PRIVATE -Wall -Wextra -Wpedantic)
    if(BUILD_TESTING)
        target_compile_options(binary_file_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_emitter_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_generator_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_graph_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(overlap_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
