add_executable(refract-host-bridge
  main.cpp
  openxr_host.cpp
  mirror_window.cpp
  pose_stream.cpp
  compositor_submit.cpp
  monado_backend.cpp
  steamvr_backend.cpp
)

target_include_directories(refract-host-bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/protocol
    ${PROJECT_SOURCE_DIR}/third_party/openxr-loader-aar/extracted/prefab/modules/headers/include
)

target_link_libraries(refract-host-bridge
  PRIVATE
    refract_protocol
)

if(UNIX AND NOT APPLE)
  target_link_libraries(refract-host-bridge PRIVATE dl)
endif()

if(WIN32)
  target_link_libraries(refract-host-bridge PRIVATE d3d11 dxgi user32 gdi32)
  include(loading_screen.cmake)
  refract_add_loading_screen(refract-host-bridge)
endif()
