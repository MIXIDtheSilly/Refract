cmake_minimum_required(VERSION 3.22)

project(android_xr_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REFRACT_BUILD_ANDROID_RUNTIME "Build Android OpenXR runtime shim" ON)
option(REFRACT_BUILD_HOST_BRIDGE "Build host bridge service" ON)
option(REFRACT_BUILD_TESTS "Build native tests" ON)

add_subdirectory(protocol)

if(REFRACT_BUILD_ANDROID_RUNTIME)
  add_subdirectory(android-runtime)
endif()

if(REFRACT_BUILD_HOST_BRIDGE)
  add_subdirectory(host-bridge)
endif()

# Linux PC viewer ("Play on PC"; viewer/ is the Windows one's own CMake project). Needs SDL3: the SDL3-devel
# package, or its headers in REFRACT_SDL3_HEADERS with the installed libSDL3.so.0.
if(UNIX AND NOT APPLE AND NOT ANDROID)
  option(REFRACT_BUILD_VIEWER "Build the Linux PC viewer (SDL3)" ON)
  set(REFRACT_SDL3_HEADERS "" CACHE PATH "Directory holding SDL3/SDL.h, if not installed system-wide")
  find_path(REFRACT_SDL3_INCLUDE SDL3/SDL.h HINTS "${REFRACT_SDL3_HEADERS}")
  find_library(REFRACT_SDL3_LIBRARY NAMES SDL3 libSDL3.so.0)
  if(REFRACT_BUILD_VIEWER AND REFRACT_SDL3_INCLUDE AND REFRACT_SDL3_LIBRARY)
    add_executable(refract-viewer viewer/viewer_linux.cpp)
    target_include_directories(refract-viewer PRIVATE "${REFRACT_SDL3_INCLUDE}")
    target_link_libraries(refract-viewer PRIVATE refract_protocol "${REFRACT_SDL3_LIBRARY}")
  elseif(REFRACT_BUILD_VIEWER)
    message(WARNING "SDL3 not found (install SDL3-devel or set REFRACT_SDL3_HEADERS): the Linux PC viewer is not built")
  endif()
endif()

# The emulator's host Vulkan layer, as tools/windows_gpu_layer builds it on Windows: shares the game's eye images
# with the viewer and host bridge (debug.refract.gpu_share=1) and repairs gfxstream's descriptor templates.
# tools/linux_android_emulator.sh loads it from build-linux/gpu-layer.
if(UNIX AND NOT APPLE AND NOT ANDROID)
  option(REFRACT_BUILD_GPU_LAYER "Build the emulator's host Vulkan layer" ON)
  set(REFRACT_VULKAN_HEADERS "" CACHE PATH "Directory holding vulkan/vulkan.h, if not installed system-wide")
  if(REFRACT_BUILD_GPU_LAYER)
    add_library(refract_gpu_layer MODULE tools/windows_gpu_layer/layer.cpp)
    target_include_directories(refract_gpu_layer PRIVATE protocol)
    if(REFRACT_VULKAN_HEADERS)
      target_include_directories(refract_gpu_layer PRIVATE "${REFRACT_VULKAN_HEADERS}")
    endif()
    target_compile_definitions(refract_gpu_layer PRIVATE VK_NO_PROTOTYPES)
    # Only the loader entry points are exported, and the layer binds to its own symbols (the emulator ships libc++).
    set_target_properties(refract_gpu_layer PROPERTIES CXX_VISIBILITY_PRESET hidden PREFIX "lib"
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gpu-layer")
    target_link_options(refract_gpu_layer PRIVATE -Wl,-Bsymbolic -Wl,--no-undefined)
    target_link_libraries(refract_gpu_layer PRIVATE pthread)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/gpu-layer/refract_gpu_layer.json" CONTENT "{\n\"file_format_version\":\"1.2.0\",\n\"layer\":{\"name\":\"VK_LAYER_REFRACT_gpu_share\",\"type\":\"GLOBAL\",\"library_path\":\"./$<TARGET_FILE_NAME:refract_gpu_layer>\",\"api_version\":\"1.3.0\",\"implementation_version\":\"1\",\"description\":\"Refract shared eye textures (opaque FD)\"}}\n")
  endif()
endif()

if(REFRACT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
