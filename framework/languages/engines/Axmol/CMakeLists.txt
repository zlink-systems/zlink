cmake_minimum_required(VERSION 3.20)
project(EngineLobby LANGUAGES CXX)

if(NOT DEFINED ENV{AX_ROOT})
  message(FATAL_ERROR "Set AX_ROOT to an Axmol 2.3.0 source tree")
endif()
set(_AX_ROOT "$ENV{AX_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${_AX_ROOT}/cmake/Modules")
include(AXBuildSet)
add_subdirectory("${_AX_ROOT}/core" "${CMAKE_BINARY_DIR}/axmol/core")
set(content_folder "${CMAKE_CURRENT_SOURCE_DIR}/Content")

find_package(zlink_framework CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_subdirectory(third_party/zlink_axmol_connector)

add_executable(EngineLobby
  Source/main.cpp
  Source/AppDelegate.cpp
  Source/EngineLobbyScene.cpp)
target_compile_features(EngineLobby PRIVATE cxx_std_20)
target_include_directories(EngineLobby PRIVATE Source)
target_link_libraries(EngineLobby PRIVATE
  ${_AX_CORE_LIB}
  zlink_axmol_stream_connector
  nlohmann_json::nlohmann_json)
ax_setup_app_config(EngineLobby)
ax_setup_app_props(EngineLobby)
