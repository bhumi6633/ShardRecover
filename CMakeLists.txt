cmake_minimum_required(VERSION 3.20)

project(ShardRecover LANGUAGES CXX)

include(CTest)

option(SHARDRECOVER_ENABLE_SANITIZERS
    "Build ShardRecover targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SHARDRECOVER_ENABLE_FUZZING
    "Build libFuzzer targets (requires Clang or AppleClang)" OFF)

if(SHARDRECOVER_ENABLE_FUZZING AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SHARDRECOVER_ENABLE_FUZZING requires Clang or AppleClang")
endif()

if(SHARDRECOVER_ENABLE_SANITIZERS OR SHARDRECOVER_ENABLE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ShardRecover sanitizer builds require GCC, Clang, or AppleClang")
    endif()
    add_library(shardrecover_sanitizers INTERFACE)
    target_compile_options(shardrecover_sanitizers INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(shardrecover_sanitizers INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shardrecover_core
    src/binary_file.cpp
    src/damage.cpp
    src/evaluation.cpp
    src/fragment_emitter.cpp
    src/fragment_generator.cpp
    src/fragment_graph.cpp
    src/fragment_loader.cpp
    src/mapped_file.cpp
    src/overlap.cpp
    src/png/analyzer.cpp
    src/png/crc.cpp
    src/png/repair.cpp
    src/png/reconstruction_evaluator.cpp
    src/repair.cpp
    src/reconstruction.cpp
    src/thread_pool.cpp
    src/trace.cpp
    src/trace_json.cpp
)
target_include_directories(shardrecover_core PUBLIC include)
if(TARGET shardrecover_sanitizers)
    target_link_libraries(shardrecover_core PUBLIC shardrecover_sanitizers)
endif()

add_executable(shardrecover
    cli/analyze_command.cpp
    cli/fragment_directory.cpp
    cli/fragment_command.cpp
    cli/main.cpp
    cli/reconstruct_command.cpp
)
target_link_libraries(shardrecover PRIVATE shardrecover_core)

add_executable(shardrecover_graph_bench benchmarks/graph_benchmark.cpp)
target_link_libraries(shardrecover_graph_bench PRIVATE shardrecover_core)

add_executable(shardrecover_recovery_bench benchmarks/recovery_benchmark.cpp)
target_link_libraries(shardrecover_recovery_bench PRIVATE shardrecover_core)

if(SHARDRECOVER_ENABLE_FUZZING)
    add_executable(shardrecover_png_fuzz fuzz/png_analyzer_fuzz.cpp)
    target_link_libraries(shardrecover_png_fuzz PRIVATE shardrecover_core)
    target_compile_options(shardrecover_png_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(shardrecover_png_fuzz PRIVATE -fsanitize=fuzzer)

    add_executable(shardrecover_overlap_fuzz fuzz/overlap_fuzz.cpp)
    target_link_libraries(shardrecover_overlap_fuzz PRIVATE shardrecover_core)
    target_compile_options(shardrecover_overlap_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(shardrecover_overlap_fuzz PRIVATE -fsanitize=fuzzer)
endif()

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

    add_executable(indexed_graph_tests tests/indexed_graph_tests.cpp)
    target_link_libraries(indexed_graph_tests PRIVATE shardrecover_core)
    add_test(NAME indexed_graph_tests COMMAND indexed_graph_tests)

    add_executable(parallel_graph_tests tests/parallel_graph_tests.cpp)
    target_link_libraries(parallel_graph_tests PRIVATE shardrecover_core)
    add_test(NAME parallel_graph_tests COMMAND parallel_graph_tests)

    add_executable(thread_pool_tests tests/thread_pool_tests.cpp)
    target_link_libraries(thread_pool_tests PRIVATE shardrecover_core)
    add_test(NAME thread_pool_tests COMMAND thread_pool_tests)

    add_executable(mapped_file_tests tests/mapped_file_tests.cpp)
    target_link_libraries(mapped_file_tests PRIVATE shardrecover_core)
    add_test(NAME mapped_file_tests COMMAND mapped_file_tests)

    add_executable(mapped_io_tests tests/mapped_io_tests.cpp)
    target_link_libraries(mapped_io_tests PRIVATE shardrecover_core)
    add_test(NAME mapped_io_tests COMMAND mapped_io_tests)

    add_executable(fuzz_robustness_tests tests/fuzz_robustness_tests.cpp)
    target_link_libraries(fuzz_robustness_tests PRIVATE shardrecover_core)
    add_test(NAME fuzz_robustness_tests COMMAND fuzz_robustness_tests)

    add_executable(overlap_tests tests/overlap_tests.cpp)
    target_link_libraries(overlap_tests PRIVATE shardrecover_core)
    add_test(NAME overlap_tests COMMAND overlap_tests)

    add_executable(reconstruction_tests tests/reconstruction_tests.cpp)
    target_link_libraries(reconstruction_tests PRIVATE shardrecover_core)
    add_test(NAME reconstruction_tests COMMAND reconstruction_tests)

    add_executable(png_analyzer_tests tests/png_analyzer_tests.cpp)
    target_link_libraries(png_analyzer_tests PRIVATE shardrecover_core)
    add_test(NAME png_analyzer_tests COMMAND png_analyzer_tests)

    add_executable(format_reconstruction_tests tests/format_reconstruction_tests.cpp)
    target_link_libraries(format_reconstruction_tests PRIVATE shardrecover_core)
    add_test(NAME format_reconstruction_tests COMMAND format_reconstruction_tests)

    add_executable(damage_tests tests/damage_tests.cpp)
    target_link_libraries(damage_tests PRIVATE shardrecover_core)
    add_test(NAME damage_tests COMMAND damage_tests)

    add_executable(corruption_tolerant_overlap_tests
        tests/corruption_tolerant_overlap_tests.cpp)
    target_link_libraries(corruption_tolerant_overlap_tests PRIVATE shardrecover_core)
    add_test(NAME corruption_tolerant_overlap_tests COMMAND corruption_tolerant_overlap_tests)

    add_executable(repair_tests tests/repair_tests.cpp)
    target_link_libraries(repair_tests PRIVATE shardrecover_core)
    add_test(NAME repair_tests COMMAND repair_tests)

    add_executable(png_repair_tests tests/png_repair_tests.cpp)
    target_link_libraries(png_repair_tests PRIVATE shardrecover_core)
    add_test(NAME png_repair_tests COMMAND png_repair_tests)

    add_executable(recovery_evaluation_tests tests/recovery_evaluation_tests.cpp)
    target_link_libraries(recovery_evaluation_tests PRIVATE shardrecover_core)
    add_test(NAME recovery_evaluation_tests COMMAND recovery_evaluation_tests)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shardrecover_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shardrecover PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shardrecover_graph_bench PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shardrecover_recovery_bench PRIVATE -Wall -Wextra -Wpedantic)
    if(BUILD_TESTING)
        target_compile_options(binary_file_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_emitter_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_generator_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fragment_graph_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(indexed_graph_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(parallel_graph_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(thread_pool_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(mapped_file_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(mapped_io_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(fuzz_robustness_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(overlap_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(reconstruction_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(png_analyzer_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(format_reconstruction_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(damage_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(corruption_tolerant_overlap_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(repair_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(png_repair_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(recovery_evaluation_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
