# Adds the loading screen (loading_screen.cpp and the embedded logo) to a Windows target.
# Used by host-bridge/ and by viewer/, which is a CMake project of its own.
function(refract_add_loading_screen target)
  set(logo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../img/Refract_logo_trans.png")
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${logo}")
  file(READ "${logo}" logo_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," logo_bytes "${logo_hex}")
  string(REGEX REPLACE "((0x..,){32})" "\\1\n" logo_bytes "${logo_bytes}")
  file(CONFIGURE OUTPUT "${generated}/refract_logo_png.h"
    CONTENT "#pragma once\n// Generated from img/Refract_logo_trans.png by host-bridge/loading_screen.cmake.\nstatic const unsigned char kRefractLogoPng[] = {\n${logo_bytes}\n};\n")
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/loading_screen.cpp")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${generated}")
  target_link_libraries(${target} PRIVATE ole32 windowscodecs)  # WIC decodes the logo.
endfunction()
