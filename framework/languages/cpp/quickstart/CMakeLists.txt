cmake_minimum_required(VERSION 3.24)
project(zlink_quickstart_cpp LANGUAGES CXX)

# Everything the two processes need comes from this one package. The prefix
# it is found under comes from bootstrap.cmake (.zlink/install) or from the
# command line; see README.md.
find_package(zlink_framework CONFIG REQUIRED)

add_executable(quickstart_server Server/main.cpp)
target_link_libraries(quickstart_server PRIVATE zlink::framework)
target_compile_features(quickstart_server PRIVATE cxx_std_20)

add_executable(quickstart_client Client/main.cpp)
target_link_libraries(quickstart_client PRIVATE zlink::framework)
target_compile_features(quickstart_client PRIVATE cxx_std_20)

foreach(_target quickstart_server quickstart_client)
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
