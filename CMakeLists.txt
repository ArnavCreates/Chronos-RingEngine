cmake_minimum_required(VERSION 3.20)
project(ChronosRingEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHRONOS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

find_package(Threads REQUIRED)
find_package(GTest REQUIRED)

function(chronos_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-O3>
                $<$<CONFIG:Release>:-march=native>
                $<$<CONFIG:Release>:-DNDEBUG>)
        endif()
    endif()

    if(CHRONOS_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(chronos_support STATIC src/engine/MatchingEngine.cpp)
target_include_directories(chronos_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(chronos_support PUBLIC Threads::Threads)
chronos_enable_warnings(chronos_support)

add_executable(chronos_mpsc_bench src/benchmark/mpsc_benchmark.cpp)
target_link_libraries(chronos_mpsc_bench PRIVATE chronos_support)
chronos_enable_warnings(chronos_mpsc_bench)

add_executable(chronos_variant_bench src/benchmark/variant_benchmark.cpp)
target_link_libraries(chronos_variant_bench PRIVATE chronos_support)
chronos_enable_warnings(chronos_variant_bench)

enable_testing()
add_executable(chronos_tests
    tests/test_engine_core.cpp
    tests/test_slab_and_ghost.cpp
    tests/test_mpsc_ring.cpp
    tests/test_differential_replay.cpp)
target_link_libraries(chronos_tests PRIVATE chronos_support GTest::gtest GTest::gtest_main Threads::Threads)
target_compile_definitions(chronos_tests PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
chronos_enable_warnings(chronos_tests)
add_test(NAME chronos_tests COMMAND chronos_tests)
set_tests_properties(chronos_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
