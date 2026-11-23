add_executable(refract-host-bridge
  main.cpp
  openxr_host.cpp
  mirror_window.cpp
  stats_overlay.cpp
  pose_stream.cpp
  compositor_submit.cpp
  monado_backend.cpp
  steamvr_backend.cpp
)

set(REFRACT_OPENXR_HEADERS "" CACHE PATH "Directory holding openxr/openxr.h")
if(NOT REFRACT_OPENXR_HEADERS)
  set(_refract_openxr_candidates
    "${PROJECT_SOURCE_DIR}/third_party/openxr-loader-aar/extracted/prefab/modules/headers/include"
    "${CMAKE_BINARY_DIR}/_deps/openxr-sdk/include"
    "${PROJECT_SOURCE_DIR}/build-windows-nvidia/_deps/openxr-sdk/include"
  )
  foreach(_candidate IN LISTS _refract_openxr_candidates)
    if(EXISTS "${_candidate}/openxr/openxr.h")
      set(REFRACT_OPENXR_HEADERS "${_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${REFRACT_OPENXR_HEADERS}/openxr/openxr.h")
  message(FATAL_ERROR "OpenXR headers not found. Run build_host.cmd or set REFRACT_OPENXR_HEADERS.")
endif()

target_include_directories(refract-host-bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/protocol
    ${PROJECT_SOURCE_DIR}/viewer  # android_stats.h, shared with the PC viewer's overlay.
    ${REFRACT_OPENXR_HEADERS}
)

target_link_libraries(refract-host-bridge
  PRIVATE
    refract_protocol
)

if(UNIX AND NOT APPLE)
  # Linux renders through Vulkan. libvulkan.so.1 and the OpenXR loader are opened at run time, so only
  # headers are needed: the openxr-devel and vulkan-headers packages, or these directories.
  set(REFRACT_OPENXR_HEADERS "" CACHE PATH "Directory holding openxr/openxr.h, if not installed system-wide")
  set(REFRACT_VULKAN_HEADERS "" CACHE PATH "Directory holding vulkan/vulkan.h, if not installed system-wide")
  target_sources(refract-host-bridge PRIVATE vulkan_renderer.cpp)
  foreach(headers IN ITEMS "${REFRACT_OPENXR_HEADERS}" "${REFRACT_VULKAN_HEADERS}")
    if(headers)
      target_include_directories(refract-host-bridge PRIVATE "${headers}")
    endif()
  endforeach()
  target_link_libraries(refract-host-bridge PRIVATE dl)
  # The preview window (a window showing the game while it plays in the headset) needs SDL3: the SDL3-devel package,
  # or its headers in REFRACT_SDL3_HEADERS with the installed libSDL3.so.0. Without it the bridge runs windowless.
  set(REFRACT_SDL3_HEADERS "" CACHE PATH "Directory holding SDL3/SDL.h, if not installed system-wide")
  find_path(REFRACT_BRIDGE_SDL3_INCLUDE SDL3/SDL.h HINTS "${REFRACT_SDL3_HEADERS}")
  find_library(REFRACT_BRIDGE_SDL3_LIBRARY NAMES SDL3 libSDL3.so.0)
  if(REFRACT_BRIDGE_SDL3_INCLUDE AND REFRACT_BRIDGE_SDL3_LIBRARY)
    target_sources(refract-host-bridge PRIVATE preview_window.cpp)
    target_include_directories(refract-host-bridge PRIVATE "${REFRACT_BRIDGE_SDL3_INCLUDE}")
    target_link_libraries(refract-host-bridge PRIVATE "${REFRACT_BRIDGE_SDL3_LIBRARY}")
    target_compile_definitions(refract-host-bridge PRIVATE REFRACT_PREVIEW_WINDOW=1)
  else()
    message(WARNING "SDL3 not found: the host bridge has no preview window")
  endif()
endif()

if(WIN32)
  target_link_libraries(refract-host-bridge PRIVATE d3d11 d3dcompiler dxgi user32 gdi32 winmm)
endif()

include(loading_screen.cmake)
refract_add_loading_screen(refract-host-bridge)
