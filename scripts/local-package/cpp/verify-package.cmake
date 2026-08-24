cmake_minimum_required(VERSION 3.10)

foreach(required_variable IN ITEMS
    ZLINK_CPP_PREFIX
    ZLINK_CORE_PREFIX
    ZLINK_CPP_VERSION
    ZLINK_CPP_BUILD_DIR)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT ZLINK_CPP_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "C++ package version must be MAJOR.MINOR.PATCH")
endif()
set(version_major "${CMAKE_MATCH_1}")
set(version_minor "${CMAKE_MATCH_2}")
set(version_patch "${CMAKE_MATCH_3}")
if(version_patch GREATER 0)
  math(EXPR mismatched_patch "${version_patch} - 1")
  set(mismatched_version
    "${version_major}.${version_minor}.${mismatched_patch}")
elseif(version_minor GREATER 0)
  math(EXPR mismatched_minor "${version_minor} - 1")
  set(mismatched_version "${version_major}.${mismatched_minor}.0")
elseif(version_major GREATER 0)
  math(EXPR mismatched_major "${version_major} - 1")
  set(mismatched_version "${mismatched_major}.0.0")
else()
  message(FATAL_ERROR "Cannot derive an older Core version from 0.0.0")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef consumer_run_id)
set(consumer_run_dir
  "${ZLINK_CPP_BUILD_DIR}/package-consumer-runs/${consumer_run_id}")
set(positive_source_dir "${consumer_run_dir}/positive-src")
set(positive_build_dir "${consumer_run_dir}/positive-build")
set(negative_source_dir "${consumer_run_dir}/negative-src")
set(negative_build_dir "${consumer_run_dir}/negative-build")
set(mismatched_core_dir "${consumer_run_dir}/mismatched-core")
file(MAKE_DIRECTORY
  "${positive_source_dir}"
  "${negative_source_dir}"
  "${mismatched_core_dir}/lib/cmake/zlink")

set(positive_cmake [=[
cmake_minimum_required(VERSION 3.10)
project(zlink_cpp_package_consumer LANGUAGES CXX)
find_package(zlink_cpp @ZLINK_CPP_VERSION@ EXACT CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE zlink::cpp)
]=])
string(CONFIGURE "${positive_cmake}" positive_cmake @ONLY)
file(WRITE "${positive_source_dir}/CMakeLists.txt" "${positive_cmake}")
file(WRITE "${positive_source_dir}/main.cpp" [=[
#include <zlink.hpp>

int main()
{
  zlink::context_t context;
  context.shutdown();
  return 0;
}
]=])

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${positive_source_dir}"
    -B "${positive_build_dir}"
    "-Dzlink_cpp_DIR=${ZLINK_CPP_PREFIX}/lib/cmake/zlink_cpp"
    "-Dzlink_DIR=${ZLINK_CORE_PREFIX}/lib/cmake/zlink"
  RESULT_VARIABLE positive_configure_result
  OUTPUT_VARIABLE positive_configure_output
  ERROR_VARIABLE positive_configure_error)
if(NOT positive_configure_result EQUAL 0)
  message(FATAL_ERROR
    "Exact C++/Core package consumer configure failed:\n"
    "${positive_configure_output}${positive_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${positive_build_dir}"
  RESULT_VARIABLE positive_build_result
  OUTPUT_VARIABLE positive_build_output
  ERROR_VARIABLE positive_build_error)
if(NOT positive_build_result EQUAL 0)
  message(FATAL_ERROR
    "Exact C++/Core package consumer build failed:\n"
    "${positive_build_output}${positive_build_error}")
endif()

file(WRITE
  "${mismatched_core_dir}/lib/cmake/zlink/zlinkConfig.cmake"
  "add_library(libzlink INTERFACE IMPORTED GLOBAL)\n")
set(mismatched_version_config [=[
set(PACKAGE_VERSION "@mismatched_version@")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
endif()
]=])
string(CONFIGURE "${mismatched_version_config}" mismatched_version_config @ONLY)
file(WRITE
  "${mismatched_core_dir}/lib/cmake/zlink/zlinkConfigVersion.cmake"
  "${mismatched_version_config}")

set(negative_cmake [=[
cmake_minimum_required(VERSION 3.10)
project(zlink_cpp_mismatched_core_consumer LANGUAGES CXX)
find_package(zlink_cpp @ZLINK_CPP_VERSION@ EXACT CONFIG QUIET)
if(zlink_cpp_FOUND)
  message(FATAL_ERROR
    "zlink_cpp @ZLINK_CPP_VERSION@ accepted mismatched Core @mismatched_version@")
endif()
]=])
string(CONFIGURE "${negative_cmake}" negative_cmake @ONLY)
file(WRITE "${negative_source_dir}/CMakeLists.txt" "${negative_cmake}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${negative_source_dir}"
    -B "${negative_build_dir}"
    "-Dzlink_cpp_DIR=${ZLINK_CPP_PREFIX}/lib/cmake/zlink_cpp"
    "-Dzlink_DIR=${mismatched_core_dir}/lib/cmake/zlink"
  RESULT_VARIABLE negative_configure_result
  OUTPUT_VARIABLE negative_configure_output
  ERROR_VARIABLE negative_configure_error)
if(NOT negative_configure_result EQUAL 0)
  message(FATAL_ERROR
    "Mismatched Core rejection consumer failed:\n"
    "${negative_configure_output}${negative_configure_error}")
endif()

set(package_config
  "${ZLINK_CPP_PREFIX}/lib/cmake/zlink_cpp/zlink_cppConfig.cmake")
if(NOT EXISTS "${package_config}")
  message(FATAL_ERROR "C++ package config is missing: ${package_config}")
endif()
file(READ "${package_config}" package_config_text)
set(exact_dependency
  "find_dependency(zlink ${ZLINK_CPP_VERSION} EXACT CONFIG)")
string(FIND "${package_config_text}" "${exact_dependency}" exact_dependency_pos)
if(exact_dependency_pos EQUAL -1)
  message(FATAL_ERROR
    "C++ package config does not require exact Core ${ZLINK_CPP_VERSION}")
endif()

message(STATUS
  "C++ package verified with Core ${ZLINK_CPP_VERSION}; "
  "Core ${mismatched_version} was rejected")
