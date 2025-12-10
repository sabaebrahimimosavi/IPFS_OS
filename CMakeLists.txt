cmake_minimum_required(VERSION 3.10)
project(c_engine C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -pthread -Wall -Wextra")

# BLAKE3 SIMD optimizations (disabled for portability)
# These disable CPU-specific optimizations for maximum compatibility
add_compile_definitions(
        BLAKE3_NO_SSE2
        BLAKE3_NO_SSE41
        BLAKE3_NO_AVX2
        BLAKE3_NO_AVX512
)

# Source files
set(SOURCES
        c_engine.c
        upload.c
        download.c
        common. c
        blake3.c
        blake3_dispatch.c
        blake3_portable.c
)

# Create executable
add_executable(c_engine ${SOURCES})

# Link pthread library
target_link_libraries(c_engine PRIVATE pthread)

# Include directories
target_include_directories(c_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Optional: Add debug symbols
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(c_engine PRIVATE -g -O0)
    message(STATUS "Building in DEBUG mode")
else()
    message(STATUS "Building in RELEASE mode")
endif()

# Optional: Print configuration
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS}")