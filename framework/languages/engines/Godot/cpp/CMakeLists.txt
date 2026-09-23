cmake_minimum_required(VERSION 3.24)
project(engine_lobby_godot LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.gdignore" "")

set(GODOT_CPP_ROOT "" CACHE PATH "Path to the godot-cpp 4.4 source checkout")
set(ZLINK_FRAMEWORK_CPP_SOURCE_ROOT "" CACHE PATH "Path to the framework C++ source release")
if(NOT EXISTS "${GODOT_CPP_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Set GODOT_CPP_ROOT to a godot-cpp 4.4 checkout")
endif()
if(NOT EXISTS "${ZLINK_FRAMEWORK_CPP_SOURCE_ROOT}/connector/engines/godot/CMakeLists.txt")
  message(FATAL_ERROR "Set ZLINK_FRAMEWORK_CPP_SOURCE_ROOT to the framework C++ source release")
endif()

find_package(zlink_stream_connector_cpp CONFIG REQUIRED)
if(MSVC)
  set(GODOTCPP_USE_STATIC_CPP OFF CACHE BOOL "Match the prebuilt connector's dynamic MSVC runtime" FORCE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "Match the prebuilt connector's dynamic MSVC runtime" FORCE)
endif()
add_subdirectory("${GODOT_CPP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
add_subdirectory("${ZLINK_FRAMEWORK_CPP_SOURCE_ROOT}/connector/engines/godot"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink-godot-stream-connector")

add_library(engine_lobby_godot SHARED src/engine_lobby_node.cpp src/register_types.cpp)
target_compile_features(engine_lobby_godot PRIVATE cxx_std_20)
target_include_directories(engine_lobby_godot PRIVATE src)
target_link_libraries(engine_lobby_godot PRIVATE godot-cpp zlink_godot_stream_connector)
set_target_properties(engine_lobby_godot PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin")
if(WIN32)
  add_custom_command(TARGET engine_lobby_godot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy -t "${CMAKE_CURRENT_SOURCE_DIR}/bin"
      $<TARGET_RUNTIME_DLLS:engine_lobby_godot>
    COMMAND_EXPAND_LISTS)
endif()
