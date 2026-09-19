cmake_minimum_required(VERSION 3.24)
project(zlink_tutorial_cpp LANGUAGES CXX)

# Everything the two processes need comes from this one package. The prefixes
# it is found under are passed on the command line; see README.md.
find_package(zlink_framework CONFIG REQUIRED)
find_package(zlink_http_client_cpp CONFIG REQUIRED)

add_executable(tutorial_server Server/main.cpp)
target_link_libraries(tutorial_server PRIVATE zlink::framework zlink::framework_locations_redis)
target_compile_features(tutorial_server PRIVATE cxx_std_20)

add_executable(tutorial_client Client/main.cpp)
target_link_libraries(tutorial_client PRIVATE zlink::framework zlink::framework_locations_redis)
target_compile_features(tutorial_client PRIVATE cxx_std_20)

# This process is outside the mesh. It links the connector only, never the
# framework.
add_executable(tutorial_stream_client StreamClient/main.cpp)
target_link_libraries(tutorial_stream_client PRIVATE zlink::stream_connector)
target_compile_features(tutorial_stream_client PRIVATE cxx_std_20)

add_executable(tutorial_http_client HttpClient/main.cpp)
target_link_libraries(tutorial_http_client PRIVATE zlink::http_client)
target_compile_features(tutorial_http_client PRIVATE cxx_std_20)

foreach(_target tutorial_server tutorial_client tutorial_stream_client tutorial_http_client)
  if(MSVC)
    # The framework headers are large enough to exhaust MSVC's default object
    # file section limit, and they carry UTF-8 source that the compiler reads
    # as the system code page without /utf-8.
    target_compile_options(${_target} PRIVATE /bigobj /utf-8)
  endif()
  if(WIN32)
    # Windows has no RPATH: the loader looks next to the executable. Core is a
    # DLL (zlink.dll) that no package manager deploys for us, so copy every
    # runtime DLL this target links, Core included, beside it.
    add_custom_command(TARGET ${_target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${_target}> $<TARGET_FILE_DIR:${_target}>
      COMMAND_EXPAND_LISTS)
  endif()
endforeach()
