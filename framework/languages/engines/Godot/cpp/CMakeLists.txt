cmake_minimum_required(VERSION 3.20)
project(engine_lobby_godot LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GODOT_CPP_ROOT "" CACHE PATH "Path to the godot-cpp 4.4 source checkout")
set(ZLINK_FRAMEWORK_CPP_ROOT "" CACHE PATH "Path to framework/languages/cpp")
if(NOT EXISTS "${GODOT_CPP_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Set GODOT_CPP_ROOT to a godot-cpp 4.4 checkout")
endif()
if(NOT EXISTS "${ZLINK_FRAMEWORK_CPP_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Set ZLINK_FRAMEWORK_CPP_ROOT to framework/languages/cpp")
endif()

add_subdirectory("${GODOT_CPP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
add_subdirectory("${ZLINK_FRAMEWORK_CPP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/zlink-framework-cpp")

add_library(engine_lobby_godot SHARED src/engine_lobby_node.cpp src/register_types.cpp)
target_compile_features(engine_lobby_godot PRIVATE cxx_std_20)
target_include_directories(engine_lobby_godot PRIVATE src)
target_link_libraries(engine_lobby_godot PRIVATE godot-cpp zlink_godot_stream_connector)
set_target_properties(engine_lobby_godot PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
