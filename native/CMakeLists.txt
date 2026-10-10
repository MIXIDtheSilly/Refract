cmake_minimum_required(VERSION 3.22)
project(refract_native LANGUAGES C CXX ASM ASM_MASM)

# Runs Android ARM64 apps directly on Windows (no emulator). See docs/native_runtime.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
if(MSVC)
  # RelWithDebInfo defaults to /Ob1; the JIT's host-side paths want full inlining.
  foreach(lang C CXX)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
  endforeach()
endif()

set(REFRACT_JDK "C:/Program Files/Java/jdk-27" CACHE PATH "JDK whose jni.h/jvmti.h are used (the JVM itself is found at run time)")
set(REFRACT_EXTERNAL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../external/src" CACHE PATH
    "Directory holding the dynarmic and boost checkouts")

# Dynarmic (ARM64 -> x86_64 JIT)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)  # FindBoost for a headers-only Boost tree
endif()
set(Boost_INCLUDE_DIR "${REFRACT_EXTERNAL_SRC}/boost" CACHE PATH "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REFRACT_EXTERNAL_SRC}/dynarmic" "${CMAKE_BINARY_DIR}/dynarmic" EXCLUDE_FROM_ALL)

# ARM astc-encoder, decompressor only (ASTC emulation for GPUs without it)
set(ASTCENC_DIR "${REFRACT_EXTERNAL_SRC}/astc-encoder/Source")
add_library(astcdec STATIC
  ${ASTCENC_DIR}/astcenc_block_sizes.cpp
  ${ASTCENC_DIR}/astcenc_color_unquantize.cpp
  ${ASTCENC_DIR}/astcenc_decompress_symbolic.cpp
  ${ASTCENC_DIR}/astcenc_entry.cpp
  ${ASTCENC_DIR}/astcenc_image.cpp
  ${ASTCENC_DIR}/astcenc_integer_sequence.cpp
  ${ASTCENC_DIR}/astcenc_mathlib.cpp
  ${ASTCENC_DIR}/astcenc_mathlib_softfloat.cpp
  ${ASTCENC_DIR}/astcenc_partition_tables.cpp
  ${ASTCENC_DIR}/astcenc_percentile_tables.cpp
  ${ASTCENC_DIR}/astcenc_quantization.cpp
  ${ASTCENC_DIR}/astcenc_symbolic_physical.cpp
  ${ASTCENC_DIR}/astcenc_weight_quant_xfer_tables.cpp
)
target_include_directories(astcdec PUBLIC ${ASTCENC_DIR})
target_compile_definitions(astcdec PRIVATE ASTCENC_DECOMPRESS_ONLY ASTCENC_NEON=0 ASTCENC_SVE=0 ASTCENC_SSE=41
  ASTCENC_AVX=0 ASTCENC_POPCNT=1 ASTCENC_F16C=0 ASTCENC_X86_GATHERS=0 _CRT_SECURE_NO_WARNINGS)

add_executable(refract_native
  src/main.cpp
  src/common.cpp
  src/memory.cpp
  src/files.cpp
  src/vfs.cpp
  src/procfs.cpp
  src/net.cpp
  src/thread.cpp
  src/hostprof.cpp
  src/syscalls.cpp
  src/emulate.cpp
  src/elf_loader.cpp
  src/thunks.cpp
  src/cpu_dynarmic.cpp
  src/vulkan_hle.cpp
  src/vk_texcomp.cpp
  src/properties.cpp
  src/inet.cpp
  src/trace.cpp
  src/android_hle.cpp
  src/audio_hle.cpp
  src/sqlite_jni.cpp
  src/jni_bridge.cpp
  src/host_runtime.cpp
  src/native_entry.asm
)
target_compile_definitions(refract_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_include_directories(refract_native PRIVATE src
  "${REFRACT_EXTERNAL_SRC}/Vulkan-Headers/include"
  "${REFRACT_JDK}/include" "${REFRACT_JDK}/include/win32")
target_link_libraries(refract_native PRIVATE dynarmic astcdec winmm bcrypt dbghelp ws2_32 ole32)
if(MSVC)
  target_compile_options(refract_native PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /utf-8 /permissive- /bigobj>)
  target_link_options(refract_native PRIVATE /STACK:8388608)
endif()
