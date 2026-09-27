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

if(REFRACT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
