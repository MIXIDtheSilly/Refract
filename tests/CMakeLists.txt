add_test(NAME refract_host_bridge_starts COMMAND refract-host-bridge)

add_executable(refract_menu_shortcut_smoke menu_shortcut_smoke.cpp)
target_link_libraries(refract_menu_shortcut_smoke PRIVATE refract_protocol)
add_test(NAME refract_menu_shortcut COMMAND refract_menu_shortcut_smoke)

add_executable(refract_runtime_smoke runtime_smoke.cpp)
target_include_directories(refract_runtime_smoke
  PRIVATE
    ${PROJECT_SOURCE_DIR}/android-runtime
)
target_link_libraries(refract_runtime_smoke PRIVATE openxr_runtime)
add_test(NAME refract_runtime_negotiates_and_frames COMMAND refract_runtime_smoke)
set_tests_properties(refract_runtime_negotiates_and_frames PROPERTIES
  ENVIRONMENT "PATH=$<TARGET_FILE_DIR:openxr_runtime>;$ENV{PATH}"
)

if(NOT WIN32)
  add_executable(refract_gpu_transport_smoke gpu_transport_smoke.cpp)
  target_link_libraries(refract_gpu_transport_smoke PRIVATE refract_protocol)
  add_test(NAME refract_gpu_transport_passes_fd COMMAND refract_gpu_transport_smoke)
endif()

# The Linux host bridge's Vulkan frame copies, on this machine's GPU (skipped without one).
if(UNIX AND NOT APPLE AND NOT ANDROID AND TARGET refract-host-bridge)
  add_executable(refract_vulkan_renderer_smoke vulkan_renderer_smoke.cpp ../host-bridge/vulkan_renderer.cpp)
  target_include_directories(refract_vulkan_renderer_smoke PRIVATE ${PROJECT_SOURCE_DIR}/host-bridge)
  foreach(headers IN ITEMS "${REFRACT_OPENXR_HEADERS}" "${REFRACT_VULKAN_HEADERS}")
    if(headers)
      target_include_directories(refract_vulkan_renderer_smoke PRIVATE "${headers}")
    endif()
  endforeach()
  target_link_libraries(refract_vulkan_renderer_smoke PRIVATE dl)
  add_test(NAME refract_vulkan_renderer_copies_frames COMMAND refract_vulkan_renderer_smoke)
  set_tests_properties(refract_vulkan_renderer_copies_frames PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()

# The emulator layer's gfxstream descriptor-template repair (built on Windows by tools/windows_gpu_layer).
if(UNIX AND NOT APPLE AND NOT ANDROID AND TARGET refract_gpu_layer)
  add_executable(refract_gfxstream_template_fix_smoke gfxstream_template_fix_smoke.cpp)
  if(REFRACT_VULKAN_HEADERS)
    target_include_directories(refract_gfxstream_template_fix_smoke PRIVATE "${REFRACT_VULKAN_HEADERS}")
  endif()
  add_test(NAME refract_gfxstream_template_offsets_repaired COMMAND refract_gfxstream_template_fix_smoke)
endif()

add_executable(refract_video_transport_smoke video_transport_smoke.cpp)
target_link_libraries(refract_video_transport_smoke PRIVATE refract_protocol)
add_test(NAME refract_video_transport_reassembles_udp_frames COMMAND refract_video_transport_smoke)

add_subdirectory(hello_xr)

if(WIN32)
  add_executable(refract_windows_gpu_transport_smoke windows_gpu_transport_smoke.cpp)
  target_link_libraries(refract_windows_gpu_transport_smoke PRIVATE refract_protocol ws2_32)
  add_test(NAME refract_windows_gpu_metadata_and_ack COMMAND refract_windows_gpu_transport_smoke)
  set_tests_properties(refract_windows_gpu_metadata_and_ack PROPERTIES TIMEOUT 10)
endif()

add_executable(refract_pose_stream_decoder_smoke pose_stream_decoder_smoke.cpp)
target_link_libraries(refract_pose_stream_decoder_smoke PRIVATE refract_protocol)
add_test(NAME refract_pose_stream_handles_fragmentation COMMAND refract_pose_stream_decoder_smoke)

# Exercise real runtime dispatch/space transforms with deterministic host input.
if(NOT ANDROID)
  add_executable(refract_runtime_input_smoke runtime_smoke.cpp runtime_input_fixture.cpp
    ../android-runtime/session.cpp ../android-runtime/game_patches.cpp ../android-runtime/openxr_dispatch/runtime_entry.cpp)
  target_include_directories(refract_runtime_input_smoke PRIVATE ${PROJECT_SOURCE_DIR}/android-runtime)
  target_compile_definitions(refract_runtime_input_smoke PRIVATE REFRACT_INPUT_FIXTURE)
  target_link_libraries(refract_runtime_input_smoke PRIVATE refract_protocol)
  add_test(NAME refract_runtime_transforms_hand_joints COMMAND refract_runtime_input_smoke)
endif()
