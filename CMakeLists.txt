cmake_minimum_required(VERSION 3.10)
project(c_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -pthread")

add_executable(c_engine
        c_engine.c
        upload.c
        download.c
        common.c
        blake3.c
        blake3_dispatch.c
        blake3_portable.c
)
