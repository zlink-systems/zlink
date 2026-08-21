cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(zlink_c_bindings LANGUAGES C CXX)
  set(ZLINK_C_STANDALONE TRUE)
else()
  set(ZLINK_C_STANDALONE FALSE)
endif()

get_filename_component(ZLINK_C_BINDINGS_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(ZLINK_REPO_ROOT_DIR "${ZLINK_C_BINDINGS_DIR}/../.." ABSOLUTE)

set(ZLINK_CORE_DIR "${ZLINK_REPO_ROOT_DIR}/core" CACHE PATH "Path to zlink core")
set(ZLINK_C_CORE_BUILD_DIR "${ZLINK_REPO_ROOT_DIR}/core/build" CACHE PATH
    "Path to an existing zlink core build directory")

function(zlink_c_resolve_core_build_layout out_dir_var out_lib_var)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_libname "libzlink.so")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_libname "libzlink.dylib")
  elseif(WIN32)
    set(_libname "zlink.dll")
  else()
    set(_libname "")
  endif()

  if(NOT _libname)
    set(${out_dir_var} "" PARENT_SCOPE)
    set(${out_lib_var} "" PARENT_SCOPE)
    return()
  endif()

  set(_candidate_dirs)
  if(CMAKE_BUILD_TYPE)
    list(APPEND _candidate_dirs
      "${ZLINK_C_CORE_BUILD_DIR}/bin/${CMAKE_BUILD_TYPE}"
      "${ZLINK_C_CORE_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}")
  endif()
  if(WIN32)
    list(APPEND _candidate_dirs
      "${ZLINK_C_CORE_BUILD_DIR}/bin/Release"
      "${ZLINK_C_CORE_BUILD_DIR}/lib/Release")
  endif()
  list(APPEND _candidate_dirs
    "${ZLINK_C_CORE_BUILD_DIR}/lib"
    "${ZLINK_C_CORE_BUILD_DIR}/bin")
  list(REMOVE_DUPLICATES _candidate_dirs)

  unset(_core_lib CACHE)
  unset(_core_lib)
  find_file(_core_lib
    NAMES "${_libname}"
    PATHS ${_candidate_dirs}
    NO_DEFAULT_PATH)
  if(_core_lib)
    get_filename_component(_core_dir "${_core_lib}" DIRECTORY)
    set(${out_dir_var} "${_core_dir}" PARENT_SCOPE)
    set(${out_lib_var} "${_core_lib}" PARENT_SCOPE)
    return()
  endif()

  set(${out_dir_var} "" PARENT_SCOPE)
  set(${out_lib_var} "" PARENT_SCOPE)
endfunction()

function(zlink_c_ensure_core_target out_target_var out_runtime_dir_var)
  if(TARGET libzlink)
    set(${out_target_var} libzlink PARENT_SCOPE)
    set(${out_runtime_dir_var} "$<TARGET_FILE_DIR:libzlink>" PARENT_SCOPE)
    return()
  endif()

  zlink_c_resolve_core_build_layout(_core_dir _core_lib)
  if(_core_lib)
    if(NOT TARGET zlink-core-build-runtime)
      add_library(zlink-core-build-runtime SHARED IMPORTED GLOBAL)
      set_target_properties(zlink-core-build-runtime PROPERTIES
        IMPORTED_LOCATION "${_core_lib}"
      )
      if(WIN32)
        set(_core_implib_dirs
          "${ZLINK_C_CORE_BUILD_DIR}/lib/Release"
          "${ZLINK_C_CORE_BUILD_DIR}/lib"
          "${_core_dir}"
          "${_core_dir}/../lib")
        if(CMAKE_BUILD_TYPE)
          list(INSERT _core_implib_dirs 0
            "${ZLINK_C_CORE_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}")
        endif()
        list(REMOVE_DUPLICATES _core_implib_dirs)
        unset(_core_implib CACHE)
        unset(_core_implib)
        find_file(_core_implib
          NAMES zlink.lib libzlink.lib
          PATHS ${_core_implib_dirs}
          NO_DEFAULT_PATH)
        if(_core_implib)
          set_target_properties(zlink-core-build-runtime PROPERTIES
            IMPORTED_IMPLIB "${_core_implib}"
          )
        endif()
      endif()
    endif()
    set(${out_target_var} zlink-core-build-runtime PARENT_SCOPE)
    set(${out_runtime_dir_var} "${_core_dir}" PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR
    "No zlink core runtime found. Build core first under "
    "${ZLINK_C_CORE_BUILD_DIR} or include bindings/c from a build that already "
    "defines libzlink.")
endfunction()

zlink_c_ensure_core_target(ZLINK_C_CORE_TARGET ZLINK_C_CORE_RUNTIME_DIR)

option(
  ENABLE_LTO
  "Enable Link-Time Optimization (LTO/IPO) for C binding targets"
  OFF
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION "${ENABLE_LTO}")

add_library(zlink_c INTERFACE)
set(ZLINK_C_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(EXISTS "${ZLINK_C_CORE_BUILD_DIR}/include/zlink.h")
  set(ZLINK_C_PUBLIC_INCLUDE_DIR "${ZLINK_C_CORE_BUILD_DIR}/include")
endif()
target_include_directories(zlink_c INTERFACE "${ZLINK_C_PUBLIC_INCLUDE_DIR}")
target_link_libraries(zlink_c INTERFACE ${ZLINK_C_CORE_TARGET})

if(NOT DEFINED ZLINK_C_REGISTER_CTEST)
  option(
    ZLINK_C_REGISTER_CTEST
    "Register C binding tests and samples in the active CTest project"
    ${ZLINK_C_STANDALONE}
  )
endif()

if(ZLINK_C_STANDALONE AND ZLINK_C_REGISTER_CTEST)
  enable_testing()
endif()

option(ZLINK_C_BUILD_SAMPLES "Build C binding samples" OFF)
if(ZLINK_C_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

option(ZLINK_C_BUILD_TESTS "Build C binding tests" OFF)
if(ZLINK_C_BUILD_TESTS)
  function(zlink_c_add_contract_test name)
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE zlink_c)
    if(UNIX OR APPLE)
      set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
      )
    endif()
    if(ZLINK_C_REGISTER_CTEST)
      add_test(NAME ${name} COMMAND ${name})
      set_tests_properties(${name} PROPERTIES
        LABELS "contract"
        TIMEOUT 30
      )
      if(WIN32)
        set_tests_properties(${name} PROPERTIES
          ENVIRONMENT "PATH=${ZLINK_C_CORE_RUNTIME_DIR};$ENV{PATH}")
      elseif(APPLE)
        set_tests_properties(${name} PROPERTIES
          ENVIRONMENT "DYLD_LIBRARY_PATH=${ZLINK_C_CORE_RUNTIME_DIR}:$ENV{DYLD_LIBRARY_PATH}")
      elseif(UNIX)
        set_tests_properties(${name} PROPERTIES
          ENVIRONMENT "LD_LIBRARY_PATH=${ZLINK_C_CORE_RUNTIME_DIR}:$ENV{LD_LIBRARY_PATH}")
      endif()
    endif()
  endfunction()

  zlink_c_add_contract_test(test_c_contract_surface)
  zlink_c_add_contract_test(test_c_common_header_version)
  zlink_c_add_contract_test(test_c_message_lifecycle)
  zlink_c_add_contract_test(test_c_option_error_contract)
  zlink_c_add_contract_test(test_c_behavior_contract)
  zlink_c_add_contract_test(test_c_failure_boundary_contract)
  zlink_c_add_contract_test(test_c_monitor_poller_service_contract)
endif()

option(ZLINK_C_BUILD_BENCHMARKS "Build C binding benchmarks" OFF)
if(ZLINK_C_BUILD_BENCHMARKS)
  add_subdirectory(perf)
endif()

option(ZLINK_C_BUILD_BENCHES "Build C binding comparison benches" OFF)
if(ZLINK_C_BUILD_BENCHES
   OR ZLINK_BUILD_BENCH_ZMQ
   OR ZLINK_BUILD_BENCH_STREAMCOMPARE
   OR ZLINK_BUILD_BENCH_ROUTER_COMPARE)
  add_subdirectory(bench)
endif()
