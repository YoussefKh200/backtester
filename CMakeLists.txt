cmake_minimum_required(VERSION 3.16)
project(QuantEngine VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ─────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3
        -march=native
        -Wall -Wextra -Wpedantic
        -ffast-math
        -funroll-loops
        -DNDEBUG
    )
elseif(MSVC)
    add_compile_options(/O2 /W4 /arch:AVX2)
endif()

# ── Source files ───────────────────────────────────────────────────────────
set(SOURCES
    main.cpp
)

# All implementation is header-only for maximum inlining
# Add .cpp files here if you split implementation out

add_executable(quant_engine ${SOURCES})

target_include_directories(quant_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Optional: link pthreads for future multi-threading ─────────────────────
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(quant_engine Threads::Threads)
endif()

# ── Optional: Google Test ──────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest)
    if(GTest_FOUND)
        add_subdirectory(tests)
    endif()
endif()

# ── Install ────────────────────────────────────────────────────────────────
install(TARGETS quant_engine DESTINATION bin)
install(DIRECTORY
    core data execution strategy portfolio risk metrics
    DESTINATION include/quant_engine
    FILES_MATCHING PATTERN "*.h"
)

message(STATUS "QuantEngine configured")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std:    ${CMAKE_CXX_STANDARD}")
