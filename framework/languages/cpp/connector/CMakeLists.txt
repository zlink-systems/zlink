cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" _zlink_framework_version_line)
list(LENGTH _zlink_framework_version_line _zlink_framework_version_line_count)
if(NOT _zlink_framework_version_line_count EQUAL 1
    OR NOT _zlink_framework_version_line MATCHES "^ZLINK_FRAMEWORK_VERSION=[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "C++ Framework VERSION must contain one ZLINK_FRAMEWORK_VERSION=X.Y.Z line")
endif()
string(REPLACE "ZLINK_FRAMEWORK_VERSION=" "" _zlink_framework_version
  "${_zlink_framework_version_line}")
project(zlink_stream_connector_cpp VERSION ${_zlink_framework_version} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

set(ZLINK_FRAMEWORK_CPP_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(ZLINK_FRAMEWORK_CPP_REPO_ROOT "${ZLINK_FRAMEWORK_CPP_DIR}/../../..")
option(ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST "Use packaged Boost instead of the repository's vendored Boost" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT "Build Stream Connector e2e client helpers" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_UNREAL "Build Unreal Stream Connector source adapter" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_GODOT "Build Godot Stream Connector source adapter" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL "Build Axmol Stream Connector source adapter" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_LZ4 "Enable Stream Connector LZ4 compression" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_TLS "Enable Stream Connector TLS transport" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET "Enable Stream Connector WebSocket transport" ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include("${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-cpp-project-defaults.cmake")
  find_package(OpenSSL QUIET)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include("${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-cpp-boost.cmake")
  include("${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-cpp-shared-library.cmake")
  function(zlink_framework_cpp_configure_msvc_target target_name)
    if(MSVC)
      target_compile_options(${target_name} PUBLIC /bigobj)
    endif()
  endfunction()
endif()

add_library(zlink_stream_connector ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
  core/src/runtime/calls/zlink_stream_calls.cpp
  core/src/runtime/connector_runtime.cpp
  core/src/runtime/protocol/compression/lz4_compression_codec.cpp
  core/src/runtime/protocol/framing/frame_codec.cpp
  core/src/runtime/protocol/framing.cpp
  core/src/runtime/protocol/header_codec.cpp
  core/src/runtime/protocol/metadata_codec.cpp
  core/src/runtime/protocol/packet_name_resolver.cpp
  core/src/runtime/transport/stream_connection.cpp
  core/src/runtime/transport/stream_transport_factory.cpp
  core/src/runtime/transport/websocket_connection.cpp)
add_library(zlink::stream_connector ALIAS zlink_stream_connector)
set_target_properties(zlink_stream_connector PROPERTIES
  EXPORT_NAME stream_connector)
zlink_framework_cpp_configure_shared_library(zlink_stream_connector)
target_compile_features(zlink_stream_connector PUBLIC cxx_std_20)
zlink_framework_cpp_configure_msvc_target(zlink_stream_connector)
target_include_directories(zlink_stream_connector PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/core/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/common/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_stream_connector PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/core/src)
zlink_framework_cpp_add_boost_headers(zlink_stream_connector)
zlink_framework_cpp_add_asio_system_libraries(zlink_stream_connector)
target_link_libraries(zlink_stream_connector
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE Threads::Threads)
if(ZLINK_STREAM_CONNECTOR_WITH_TLS AND OpenSSL_FOUND)
  target_compile_definitions(zlink_stream_connector PRIVATE
    ZLINK_STREAM_CONNECTOR_WITH_OPENSSL)
  target_link_libraries(zlink_stream_connector PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto)
  set(ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY
    "find_dependency(OpenSSL)")
else()
  set(ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY "")
endif()
if(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET)
  target_compile_definitions(zlink_stream_connector PRIVATE
    ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET)
endif()
if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
  find_package(lz4 REQUIRED)
  target_link_libraries(zlink_stream_connector PRIVATE lz4::lz4)
  set(ZLINK_STREAM_CONNECTOR_EXPORT_LZ4_DEPENDENCY "find_dependency(lz4)")
endif()
target_compile_definitions(zlink_stream_connector PRIVATE
  $<$<BOOL:${ZLINK_STREAM_CONNECTOR_WITH_LZ4}>:ZLINK_STREAM_CONNECTOR_WITH_LZ4>)

add_library(zlink_stream_connector_codecs INTERFACE)
add_library(zlink::stream_connector_codecs ALIAS zlink_stream_connector_codecs)
set_target_properties(zlink_stream_connector_codecs PROPERTIES
  EXPORT_NAME stream_connector_codecs)
target_compile_features(zlink_stream_connector_codecs INTERFACE cxx_std_20)
target_include_directories(zlink_stream_connector_codecs INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/common/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_stream_connector_codecs INTERFACE
  zlink::stream_connector
  nlohmann_json::nlohmann_json)

if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  add_library(zlink_stream_e2e_client INTERFACE)
  add_library(zlink::stream_e2e_client ALIAS zlink_stream_e2e_client)
  set_target_properties(zlink_stream_e2e_client PROPERTIES
    EXPORT_NAME stream_e2e_client)
  target_compile_features(zlink_stream_e2e_client INTERFACE cxx_std_20)
  target_include_directories(zlink_stream_e2e_client INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/e2e-client/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(zlink_stream_e2e_client INTERFACE
    zlink::stream_connector
    zlink::stream_connector_codecs)
endif()

add_library(zlink_stream_connector_throwing INTERFACE)
add_library(zlink::stream_connector_throwing ALIAS zlink_stream_connector_throwing)
set_target_properties(zlink_stream_connector_throwing PROPERTIES
  EXPORT_NAME stream_connector_throwing)
target_compile_features(zlink_stream_connector_throwing INTERFACE cxx_std_20)
target_include_directories(zlink_stream_connector_throwing INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/throwing-adapter/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_stream_connector_throwing INTERFACE
  zlink::stream_connector)

if(ZLINK_STREAM_CONNECTOR_BUILD_UNREAL)
  add_library(zlink_unreal_stream_connector STATIC
    engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp)
  set_target_properties(zlink_unreal_stream_connector PROPERTIES
    EXPORT_NAME unreal_stream_connector)
  target_compile_features(zlink_unreal_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_unreal_stream_connector PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/engines/unreal/Source/ZLinkStreamConnector/Public>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ZLinkStreamConnector>)
  target_link_libraries(zlink_unreal_stream_connector PRIVATE
    zlink::stream_connector
    zlink::stream_connector_codecs)
  zlink_framework_cpp_add_asio_system_libraries(zlink_unreal_stream_connector)
  if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
    target_link_libraries(zlink_unreal_stream_connector PRIVATE lz4::lz4)
  endif()
  target_compile_definitions(zlink_unreal_stream_connector PRIVATE
    $<$<BOOL:${ZLINK_STREAM_CONNECTOR_WITH_LZ4}>:ZLINK_UNREAL_STREAM_CONNECTOR_WITH_LZ4>)
endif()

if(ZLINK_STREAM_CONNECTOR_BUILD_GODOT)
  add_library(zlink_godot_stream_connector STATIC
    engines/godot/src/zlink_godot_stream_connector.cpp)
  target_compile_features(zlink_godot_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_godot_stream_connector PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/engines/godot/include>)
  target_link_libraries(zlink_godot_stream_connector
    PUBLIC zlink::stream_connector
    PRIVATE zlink::stream_connector_codecs)
endif()

if(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL)
  add_library(zlink_axmol_stream_connector STATIC
    engines/axmol/src/zlink_axmol_stream_connector.cpp)
  target_compile_features(zlink_axmol_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_axmol_stream_connector PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/engines/axmol/include>)
  target_link_libraries(zlink_axmol_stream_connector
    PUBLIC zlink::stream_connector
    PRIVATE zlink::stream_connector_codecs)
endif()


install(TARGETS
  zlink_stream_connector
  zlink_stream_connector_codecs
  zlink_stream_connector_throwing
  EXPORT zlink_stream_connector_cppTargets
  COMPONENT StreamConnector)
if(TARGET zlink_unreal_stream_connector)
  install(TARGETS zlink_unreal_stream_connector
    EXPORT zlink_stream_connector_cppTargets COMPONENT StreamConnector)
endif()
if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  install(TARGETS zlink_stream_e2e_client
    EXPORT zlink_stream_connector_cppTargets COMPONENT StreamConnector)
endif()
install(DIRECTORY core/include/ "${ZLINK_FRAMEWORK_CPP_DIR}/common/include/"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT StreamConnector FILES_MATCHING PATTERN "*.hpp")
install(FILES "${ZLINK_FRAMEWORK_CPP_DIR}/framework/include/zlink/framework/codecs/json_stream_connector.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zlink/framework/codecs
  COMPONENT StreamConnector)
if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  install(FILES "${ZLINK_FRAMEWORK_CPP_DIR}/framework/include/zlink/framework/codecs/json_stream_e2e_client.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zlink/framework/codecs
    COMPONENT StreamConnector)
  install(DIRECTORY e2e-client/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT StreamConnector FILES_MATCHING PATTERN "*.hpp")
endif()
install(DIRECTORY throwing-adapter/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT StreamConnector FILES_MATCHING PATTERN "*.hpp")
if(TARGET zlink_unreal_stream_connector)
  install(DIRECTORY engines/unreal/Source/ZLinkStreamConnector/Public/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZLinkStreamConnector
    COMPONENT StreamConnector FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()
install(EXPORT zlink_stream_connector_cppTargets
  NAMESPACE zlink::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp
  COMPONENT StreamConnector)

set(ZLINK_STREAM_CONNECTOR_CONFIG_IMPLEMENTATION_DEPENDENCIES "")
if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
  string(CONCAT ZLINK_STREAM_CONNECTOR_CONFIG_IMPLEMENTATION_DEPENDENCIES
    "find_dependency(Boost)\n"
    "${ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY}\n"
    "set(_zlink_saved_module_path \"\${CMAKE_MODULE_PATH}\")\n"
    "list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}\")\n"
    "${ZLINK_STREAM_CONNECTOR_EXPORT_LZ4_DEPENDENCY}\n"
    "set(CMAKE_MODULE_PATH \"\${_zlink_saved_module_path}\")\n"
    "unset(_zlink_saved_module_path)")
endif()
install(FILES "${ZLINK_FRAMEWORK_CPP_DIR}/cmake/Findlz4.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp
  COMPONENT StreamConnector)
configure_package_config_file(
  "${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink_stream_connector_cppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_stream_connector_cppConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zlink_stream_connector_cppConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp
  COMPONENT StreamConnector)

add_subdirectory(core)
add_subdirectory(e2e-client)
