if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_CONFIGURATION
    OR ZLINK_FRAMEWORK_CPP_CONFIGURATION STREQUAL "")
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_CONFIGURATION is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG is required")
endif()
foreach(required_variable IN ITEMS
    ZLINK_FRAMEWORK_CPP_UNREAL_LIBRARY_PATH
    ZLINK_FRAMEWORK_CPP_STREAM_LIBRARY_PATH
    ZLINK_FRAMEWORK_CPP_BINDING_LIBRARY_PATH
    ZLINK_FRAMEWORK_CPP_CORE_RUNTIME_PATH
    ZLINK_FRAMEWORK_CPP_FRAMEWORK_LIBRARY_PATH
    ZLINK_FRAMEWORK_CPP_HTTP_CLIENT_LIBRARY_PATH)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef consumer_run_id)
set(consumer_run_dir
  "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/stream-connector-consumer-runs/${consumer_run_id}")
set(consumer_install_prefix "${consumer_run_dir}/install")
set(consumer_source_dir "${consumer_run_dir}/src")
set(consumer_build_dir "${consumer_run_dir}/build")
file(MAKE_DIRECTORY "${consumer_source_dir}")

set(consumer_dependency_prefixes ${ZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH})
if(DEFINED ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX)
  list(REMOVE_ITEM consumer_dependency_prefixes
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}")
endif()
set(consumer_prefix_path "${consumer_install_prefix}")
foreach(dependency_prefix IN LISTS consumer_dependency_prefixes)
  list(APPEND consumer_prefix_path "${dependency_prefix}")
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
    --prefix "${consumer_install_prefix}"
    --component StreamConnector
    --config "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "stream connector component install failed")
endif()

set(connector_config_file
  "${consumer_install_prefix}/lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppConfig.cmake")
set(connector_targets_file
  "${consumer_install_prefix}/lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppTargets.cmake")
foreach(path IN ITEMS "${connector_config_file}" "${connector_targets_file}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "stream connector component file is missing: ${path}")
  endif()
endforeach()
foreach(forbidden_config IN ITEMS
    lib/cmake/zlink/zlinkConfig.cmake
    lib/cmake/zlink_cpp/zlink_cppConfig.cmake
    share/zlink/zlinkConfig.cmake
    share/zlink_cpp/zlink_cppConfig.cmake)
  if(EXISTS "${consumer_install_prefix}/${forbidden_config}")
    message(FATAL_ERROR "stream connector component contains: ${forbidden_config}")
  endif()
endforeach()

file(READ "${connector_config_file}" connector_config_text)
file(READ "${connector_targets_file}" connector_targets_text)
foreach(forbidden_text IN ITEMS
    "find_dependency(zlink "
    "find_dependency(zlink)"
    "find_dependency(zlink_cpp")
  string(FIND "${connector_config_text}" "${forbidden_text}" forbidden_pos)
  if(NOT forbidden_pos EQUAL -1)
    message(FATAL_ERROR "stream connector package config must not include ${forbidden_text}")
  endif()
endforeach()
if(connector_targets_text MATCHES "zlink::cpp")
  message(FATAL_ERROR "stream connector export must not link zlink::cpp")
endif()
if(connector_targets_text MATCHES "ZLINK_LZ4_LIBRARY"
    OR connector_targets_text MATCHES "vcpkg_installed/.*/liblz4")
  message(FATAL_ERROR
    "stream connector export contains a producer-specific LZ4 path")
endif()
if(EXISTS "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_FRAMEWORK_LIBRARY_PATH}"
    OR EXISTS "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_HTTP_CLIENT_LIBRARY_PATH}")
  message(FATAL_ERROR
    "stream connector component contains unrelated framework artifacts")
endif()
foreach(required_path IN ITEMS
    "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_UNREAL_LIBRARY_PATH}"
    "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_STREAM_LIBRARY_PATH}")
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "stream connector component is missing: ${required_path}")
  endif()
endforeach()
foreach(forbidden_path IN ITEMS
    "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_BINDING_LIBRARY_PATH}"
    "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_CORE_RUNTIME_PATH}")
  if(EXISTS "${forbidden_path}")
    message(FATAL_ERROR "stream connector component contains: ${forbidden_path}")
  endif()
endforeach()
if(DEFINED ZLINK_FRAMEWORK_CPP_CORE_LINK_LIBRARY_PATH
    AND EXISTS
      "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_CORE_LINK_LIBRARY_PATH}")
  message(FATAL_ERROR
    "stream connector component contains: "
    "${consumer_install_prefix}/${ZLINK_FRAMEWORK_CPP_CORE_LINK_LIBRARY_PATH}")
endif()

file(WRITE "${consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(zlink_stream_connector_consumer LANGUAGES CXX)

find_package(zlink_stream_connector_cpp CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE zlink::stream_connector)
]=])
file(WRITE "${consumer_source_dir}/main.cpp" [=[
#include <zlink/stream_connector.hpp>

int main ()
{
  auto connector = zlink::stream_connector::connector_factory_t::create (
    zlink::stream_connector::connector_options_t {});
  (void) connector;
  return 0;
}
]=])

set(consumer_configure_command
  "${CMAKE_COMMAND}" -S "${consumer_source_dir}" -B "${consumer_build_dir}"
  "-DCMAKE_PREFIX_PATH=${consumer_prefix_path}")
if(NOT ZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG)
  list(APPEND consumer_configure_command
    "-DCMAKE_BUILD_TYPE=${ZLINK_FRAMEWORK_CPP_CONFIGURATION}")
endif()
execute_process(
  COMMAND ${consumer_configure_command}
  RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "relocated stream connector consumer configure failed")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build_dir}"
    --config "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}"
  RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "relocated stream connector consumer build failed")
endif()
