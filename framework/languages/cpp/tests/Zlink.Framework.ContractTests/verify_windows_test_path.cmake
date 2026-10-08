if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR OR
    NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "C++ source and build directories are required")
endif()

# Configure the real registration function without compiling or starting an exe.
file(READ "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/CMakeLists.txt" project_text)
string(FIND "${project_text}" "function(zlink_framework_cpp_add_test)" function_start)
if(function_start EQUAL -1)
  message(FATAL_ERROR "CTest registration function was not found")
endif()
string(SUBSTRING "${project_text}" ${function_start} -1 registration)
string(FIND "${registration}" "endfunction()" function_end)
math(EXPR function_length "${function_end} + 13")
string(SUBSTRING "${registration}" 0 ${function_length} registration)

set(fixture_root "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/windows-test-path")
set(core_bin "${fixture_root}/core/bin")
set(binding_bin "${fixture_root}/binding/bin")
set(vcpkg_root "${fixture_root}/vcpkg installed")
set(inherited_path "${fixture_root}/inherited one;${fixture_root}/inherited two;$ENV{PATH}")
set(ENV{PATH} "${inherited_path}")
foreach(case IN ITEMS absent root_only triplet_only complete)
  set(case_dir "${fixture_root}/${case}")
  set(vcpkg_setup "")
  if(case STREQUAL "root_only" OR case STREQUAL "complete")
    string(APPEND vcpkg_setup "set(VCPKG_INSTALLED_DIR [==[${vcpkg_root}]==])\n")
  endif()
  if(case STREQUAL "triplet_only" OR case STREQUAL "complete")
    string(APPEND vcpkg_setup "set(VCPKG_TARGET_TRIPLET x64-windows)\n")
  endif()
  file(MAKE_DIRECTORY "${case_dir}")
  file(WRITE "${case_dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(windows_test_path NONE)\n"
    "enable_testing()\n"
    "set(WIN32 TRUE)\n"
    "set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX [==[${fixture_root}/core]==])\n"
    "set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX [==[${fixture_root}/binding]==])\n"
    "${vcpkg_setup}${registration}\n"
    "zlink_framework_cpp_add_test(NAME path_probe COMMAND nonexistent_exe LABELS framework-contract TIMEOUT 1 ENVIRONMENT PROBE=value)\n"
    "get_property(test_environment TEST path_probe PROPERTY ENVIRONMENT)\n"
    "file(WRITE \"\${CMAKE_BINARY_DIR}/environment.txt\" \"\${test_environment}\")\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${case_dir}" -B "${case_dir}/build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${case}: configure failed: ${output}${error}")
  endif()
  file(READ "${case_dir}/build/environment.txt" test_environment)
  list(GET test_environment 0 actual)
  set(expected "PATH=${core_bin};${binding_bin}")
  if(case STREQUAL "complete")
    string(APPEND expected ";${vcpkg_root}/x64-windows/bin")
  endif()
  string(APPEND expected ";${inherited_path}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${case}: unexpected PATH\nexpected: ${expected}\nactual: ${actual}")
  endif()
  list(GET test_environment 1 custom_environment)
  list(LENGTH test_environment environment_count)
  if(NOT environment_count EQUAL 2 OR NOT custom_environment STREQUAL "PROBE=value")
    message(FATAL_ERROR "${case}: custom test environment was not preserved")
  endif()
  message(STATUS "${case}: Windows test PATH passed (configure only)")
endforeach()
