cmake_minimum_required(VERSION 3.22)

project(refract_ovrplatform LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "The Platform SDK stand-in is loaded into arm64 games; build it with -DANDROID_ABI=arm64-v8a")
endif()

add_library(refract_ovrplatform SHARED ovr_platform.cpp stub_pool.S)
target_compile_options(refract_ovrplatform PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_libraries(refract_ovrplatform PRIVATE log aaudio)
target_link_options(refract_ovrplatform PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL -static-libstdc++)
