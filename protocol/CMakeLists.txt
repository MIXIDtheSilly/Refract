add_library(refract_protocol STATIC
  gpu_transport.cpp
  image_transport.cpp
  transport_tcp.cpp
  transport_shm.cpp
  video_transport.cpp
  clock_sync.cpp
)

target_include_directories(refract_protocol
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
  target_link_libraries(refract_protocol PUBLIC ws2_32)
endif()
