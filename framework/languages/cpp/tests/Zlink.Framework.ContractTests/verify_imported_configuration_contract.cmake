if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_TEST_GENERATOR
    OR ZLINK_FRAMEWORK_CPP_TEST_GENERATOR STREQUAL "")
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_TEST_GENERATOR is required")
endif()

set(helper
  "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/cmake/zlink_framework_cpp_imported_config.cmake")
set(test_root
  "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/imported-configuration-contract")
set(test_source "${test_root}/source")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_source}")

file(WRITE "${test_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(zlink_imported_configuration_contract NONE)
include("${ZLINK_IMPORTED_CONFIGURATION_HELPER}")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo;MinSizeRel")

if(ZLINK_TEST_SCENARIO STREQUAL "source-target")
  add_library(zlink_cpp INTERFACE)
  zlink_framework_cpp_configure_imported_target(zlink_cpp)
  get_target_property(release_map zlink_cpp MAP_IMPORTED_CONFIG_RELEASE)
  if(release_map)
    message(FATAL_ERROR "source target unexpectedly received an imported configuration map")
  endif()
  return()
endif()

add_library(zlink_cpp STATIC IMPORTED GLOBAL)
if(ZLINK_TEST_SCENARIO STREQUAL "release-only")
  set_property(TARGET zlink_cpp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
  set_target_properties(zlink_cpp PROPERTIES
    IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlink_cpp_release.lib")
else()
  set_property(TARGET zlink_cpp APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
  set_target_properties(zlink_cpp PROPERTIES
    IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlink_cpp_debug.lib")
  if(ZLINK_TEST_SCENARIO STREQUAL "debug-release")
    set_property(TARGET zlink_cpp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(zlink_cpp PROPERTIES
      IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlink_cpp_release.lib")
  endif()
endif()

zlink_framework_cpp_configure_imported_target(zlink_cpp)

set(expected_mapped_configurations Release RelWithDebInfo MinSizeRel)
if(ZLINK_TEST_SCENARIO STREQUAL "debug-release")
  get_target_property(release_map zlink_cpp MAP_IMPORTED_CONFIG_RELEASE)
  if(release_map)
    message(FATAL_ERROR
      "available Release configuration unexpectedly maps to '${release_map}'")
  endif()
  set(expected_mapped_configurations RelWithDebInfo MinSizeRel)
endif()
foreach(configuration IN LISTS expected_mapped_configurations)
  string(TOUPPER "${configuration}" configuration_upper)
  get_target_property(configuration_map zlink_cpp
    "MAP_IMPORTED_CONFIG_${configuration_upper}")
  if(NOT configuration_map STREQUAL "Debug")
    message(FATAL_ERROR
      "inactive ${configuration} configuration maps to '${configuration_map}', not Debug")
  endif()
endforeach()
get_target_property(debug_map zlink_cpp MAP_IMPORTED_CONFIG_DEBUG)
if(debug_map)
  message(FATAL_ERROR "requested Debug configuration must not be mapped")
endif()
]=])

function(run_configure scenario build_type expect_success)
  set(build_dir "${test_root}/${scenario}")
  set(generator_arguments -G "${ZLINK_FRAMEWORK_CPP_TEST_GENERATOR}")
  if(DEFINED ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_PLATFORM
      AND NOT ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_arguments
      -A "${ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_PLATFORM}")
  endif()
  if(DEFINED ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_TOOLSET
      AND NOT ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_arguments
      -T "${ZLINK_FRAMEWORK_CPP_TEST_GENERATOR_TOOLSET}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${test_source}"
      -B "${build_dir}"
      ${generator_arguments}
      -D "ZLINK_IMPORTED_CONFIGURATION_HELPER=${helper}"
      -D "ZLINK_TEST_SCENARIO=${scenario}"
      -D "CMAKE_BUILD_TYPE=${build_type}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
  set(configure_log "${configure_output}\n${configure_error}")

  if(expect_success AND NOT configure_result EQUAL 0)
    message(FATAL_ERROR
      "${scenario} configure unexpectedly failed:\n${configure_log}")
  endif()
  if(NOT expect_success AND configure_result EQUAL 0)
    message(FATAL_ERROR "${scenario} configure unexpectedly succeeded")
  endif()
  if(NOT expect_success
      AND (NOT configure_log MATCHES "does not provide the requested"
        OR NOT configure_log MATCHES "'Debug'"))
    message(FATAL_ERROR
      "${scenario} did not report the missing Debug configuration:\n${configure_log}")
  endif()
endfunction()

run_configure(debug-only Debug TRUE)
run_configure(debug-release Debug TRUE)
run_configure(release-only Debug FALSE)
run_configure(source-target Debug TRUE)
