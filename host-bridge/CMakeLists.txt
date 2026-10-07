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

target_include_directories(refract-host-bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/protocol
    ${PROJECT_SOURCE_DIR}/viewer  # android_stats.h, shared with the PC viewer's overlay.
    ${PROJECT_SOURCE_DIR}/third_party/openxr-loader-aar/extracted/prefab/modules/headers/include
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
endif()

if(WIN32)
  target_link_libraries(refract-host-bridge PRIVATE d3d11 d3dcompiler dxgi user32 gdi32 winmm)
  include(loading_screen.cmake)
  refract_add_loading_screen(refract-host-bridge)
endif()
