cmake_minimum_required(VERSION 3.10)
project(zlink_cpp VERSION 11.2.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

get_filename_component(ZLINK_CPP_BINDINGS_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(ZLINK_REPO_ROOT_DIR "${ZLINK_CPP_BINDINGS_DIR}/../.." ABSOLUTE)

set(ZLINK_CORE_DIR "${ZLINK_REPO_ROOT_DIR}/core" CACHE PATH "Path to zlink core")
set(ZLINK_CPP_CORE_BUILD_DIR "${ZLINK_REPO_ROOT_DIR}/core/build" CACHE PATH
    "Path to an existing zlink core build directory")
set(ZLINK_CPP_CORE_PACKAGE_PREFIX "" CACHE PATH
    "Path to an installed Zlink 11 Core package")
option(ZLINK_CPP_USE_CORE_BUILD_RUNTIME
  "Require the targeted core/build runtime instead of an installed Core package"
  OFF)
set(ZLINK_CPP_STANDALONE FALSE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(ZLINK_CPP_STANDALONE TRUE)
endif()

function(zlink_cpp_resolve_core_build_layout out_dir_var out_lib_var)
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

  set(_candidate_dirs
    "${ZLINK_CPP_CORE_BUILD_DIR}/lib"
    "${ZLINK_CPP_CORE_BUILD_DIR}/bin")
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

function(zlink_cpp_ensure_core_target out_target_var out_runtime_dir_var)
  if(TARGET libzlink)
    set(${out_target_var} libzlink PARENT_SCOPE)
    set(${out_runtime_dir_var} "$<TARGET_FILE_DIR:libzlink>" PARENT_SCOPE)
    return()
  endif()

  if(ZLINK_CPP_USE_CORE_BUILD_RUNTIME)
    zlink_cpp_resolve_core_build_layout(_core_build_dir _core_build_lib)
    if(_core_build_lib)
      if(NOT TARGET zlink-core-build-runtime)
        add_library(zlink-core-build-runtime SHARED IMPORTED GLOBAL)
        set_target_properties(zlink-core-build-runtime PROPERTIES
          IMPORTED_LOCATION "${_core_build_lib}"
          INTERFACE_INCLUDE_DIRECTORIES "${ZLINK_CORE_DIR}/include"
        )
      endif()
      set(${out_target_var} zlink-core-build-runtime PARENT_SCOPE)
      set(${out_runtime_dir_var} "${_core_build_dir}" PARENT_SCOPE)
      return()
    endif()
    message(FATAL_ERROR
      "ZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON requires libzlink under "
      "${ZLINK_CPP_CORE_BUILD_DIR}/lib or ${ZLINK_CPP_CORE_BUILD_DIR}/bin")
  endif()

  if(ZLINK_CPP_CORE_PACKAGE_PREFIX)
    find_package(zlink 11 CONFIG REQUIRED
      PATHS "${ZLINK_CPP_CORE_PACKAGE_PREFIX}"
      NO_DEFAULT_PATH)
    if(NOT TARGET libzlink)
      message(FATAL_ERROR "The installed Core package does not export libzlink")
    endif()
    get_target_property(_package_runtime libzlink IMPORTED_LOCATION)
    get_filename_component(_package_runtime_dir "${_package_runtime}" DIRECTORY)
    set(${out_target_var} libzlink PARENT_SCOPE)
    set(${out_runtime_dir_var} "${_package_runtime_dir}" PARENT_SCOPE)
    return()
  endif()

  zlink_cpp_resolve_core_build_layout(_core_build_dir _core_build_lib)
  if(_core_build_lib)
    if(NOT TARGET zlink-core-build-runtime)
      add_library(zlink-core-build-runtime SHARED IMPORTED GLOBAL)
      set_target_properties(zlink-core-build-runtime PROPERTIES
        IMPORTED_LOCATION "${_core_build_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZLINK_CORE_DIR}/include"
      )
    endif()
    set(${out_target_var} zlink-core-build-runtime PARENT_SCOPE)
    set(${out_runtime_dir_var} "${_core_build_dir}" PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR
    "No zlink core runtime found for this platform. "
    "Expected an installed Core 11 package, an existing libzlink target, "
    "or a targeted source build under ${ZLINK_CPP_CORE_BUILD_DIR}.")
endfunction()

zlink_cpp_ensure_core_target(ZLINK_CPP_CORE_TARGET ZLINK_CPP_RUNTIME_DIR)

add_library(zlink_cpp STATIC
  src/Runtime/Core/capability.cpp
  src/Runtime/Core/context.cpp
  src/Runtime/Core/utilities.cpp
  src/Runtime/Errors/error.cpp
  src/Runtime/Eventing/poller.cpp
  src/Runtime/Eventing/monitor.cpp
  src/Runtime/Eventing/timers.cpp
  src/Runtime/Messaging/message.cpp
  src/Runtime/Messaging/received.cpp
  src/Runtime/Messaging/reply_operations.cpp
  src/Runtime/Messaging/request_reply.cpp
  src/Runtime/Messaging/send_operations.cpp
  src/Runtime/Options/dealer_socket_options.cpp
  src/Runtime/Options/pubsub_socket_options.cpp
  src/Runtime/Options/router_socket_options.cpp
  src/Runtime/Options/socket_options.cpp
  src/Runtime/Options/stream_socket_options.cpp
  src/Runtime/Sockets/socket.cpp
  src/Runtime/Sockets/dealer.cpp
  src/Runtime/Sockets/pair.cpp
  src/Runtime/Sockets/proxy.cpp
  src/Runtime/Sockets/pubsub.cpp
  src/Runtime/Sockets/router.cpp
  src/Runtime/Sockets/stream.cpp
)
add_library(zlink::cpp ALIAS zlink_cpp)

target_include_directories(zlink_cpp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)
target_link_libraries(zlink_cpp PUBLIC
  $<BUILD_INTERFACE:${ZLINK_CPP_CORE_TARGET}>
  $<INSTALL_INTERFACE:libzlink>)
target_compile_features(zlink_cpp PUBLIC cxx_std_20)
set_target_properties(zlink_cpp PROPERTIES
  EXPORT_NAME cpp
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF
)

install(TARGETS zlink_cpp
  EXPORT zlink_cppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp"
)
install(EXPORT zlink_cppTargets
  NAMESPACE zlink::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_cpp
)
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink_cppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_cppConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_cpp
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_cppConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_cppConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_cppConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_cpp
)

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  add_custom_target(zlink_cpp_api_docs
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Generating zlink C++ binding API reference"
    VERBATIM
  )
endif()

if(NOT DEFINED ZLINK_CPP_BUILD_TESTS)
  option(ZLINK_CPP_BUILD_TESTS "Build C++ binding tests" OFF)
endif()

if(NOT DEFINED ZLINK_CPP_BUILD_SAMPLES)
  option(ZLINK_CPP_BUILD_SAMPLES "Build C++ binding samples" OFF)
endif()

if(NOT DEFINED ZLINK_CPP_REGISTER_CTEST)
  option(
    ZLINK_CPP_REGISTER_CTEST
    "Register C++ binding tests and samples in the active CTest project"
    ${ZLINK_CPP_STANDALONE}
  )
endif()

if(ZLINK_CPP_STANDALONE AND ZLINK_CPP_REGISTER_CTEST)
  enable_testing()
endif()

if(ZLINK_CPP_BUILD_TESTS OR ZLINK_CPP_BUILD_SAMPLES)
  function(add_cpp_binding_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink_cpp)
    if(UNIX AND NOT APPLE)
      set_target_properties(${target_name} PROPERTIES
        BUILD_RPATH "${ZLINK_CPP_RUNTIME_DIR}"
      )
    elseif(APPLE)
      set_target_properties(${target_name} PROPERTIES
        BUILD_RPATH "${ZLINK_CPP_RUNTIME_DIR}"
      )
    endif()
    if(MSVC)
      target_compile_options(${target_name} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${target_name} PRIVATE -UNDEBUG)
    endif()
  endfunction()

  function(configure_cpp_binding_runtime target_name)
    set_tests_properties(${target_name} PROPERTIES
      WORKING_DIRECTORY ${ZLINK_REPO_ROOT_DIR})
    if(WIN32)
      set_tests_properties(${target_name} PROPERTIES
        ENVIRONMENT "PATH=${ZLINK_CPP_RUNTIME_DIR};$ENV{PATH}")
    elseif(APPLE)
      set_tests_properties(${target_name} PROPERTIES
        ENVIRONMENT "DYLD_LIBRARY_PATH=${ZLINK_CPP_RUNTIME_DIR}:$ENV{DYLD_LIBRARY_PATH}")
    elseif(UNIX)
      set_tests_properties(${target_name} PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${ZLINK_CPP_RUNTIME_DIR}:$ENV{LD_LIBRARY_PATH}")
    endif()
  endfunction()
endif()

if(ZLINK_CPP_BUILD_SAMPLES)
  function(add_cpp_sample target_name source_file)
    add_cpp_binding_executable(${target_name} ${source_file})
    if(ZLINK_CPP_REGISTER_CTEST)
      if(UNIX AND NOT WIN32)
        add_test(NAME sample_smoke_${target_name}
          COMMAND /bin/bash -lc "$<TARGET_FILE:${target_name}>")
      else()
        add_test(NAME sample_smoke_${target_name} COMMAND ${target_name})
      endif()
      set_tests_properties(sample_smoke_${target_name} PROPERTIES
        LABELS "sample-smoke"
        TIMEOUT 120
      )
      configure_cpp_binding_runtime(sample_smoke_${target_name})
    endif()
  endfunction()

  set(ZLINK_CPP_SAMPLE_SOURCES
    pair_recv_sample.cpp
    pubsub_recv_sample.cpp
    dealer_router_recv_sample.cpp
    request_reply_async_sample.cpp
    stream_recv_sample.cpp
    stream_packet_callback_sample.cpp
    monitor_recv_sample.cpp
  )
  file(GLOB ZLINK_CPP_SAMPLE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/*_sample.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/*_example.cpp"
  )
  set(ZLINK_CPP_UNREGISTERED_SAMPLE_SOURCES)
  foreach(sample_file IN LISTS ZLINK_CPP_SAMPLE_FILES)
    get_filename_component(sample_source "${sample_file}" NAME)
    list(FIND ZLINK_CPP_SAMPLE_SOURCES "${sample_source}" sample_index)
    if(sample_index EQUAL -1)
      list(APPEND ZLINK_CPP_UNREGISTERED_SAMPLE_SOURCES "${sample_source}")
    endif()
  endforeach()
  if(ZLINK_CPP_UNREGISTERED_SAMPLE_SOURCES)
    list(JOIN ZLINK_CPP_UNREGISTERED_SAMPLE_SOURCES ", " unregistered_samples)
    message(FATAL_ERROR
      "C++ sample source files are not registered in "
      "ZLINK_CPP_SAMPLE_SOURCES: ${unregistered_samples}")
  endif()
  foreach(sample_source IN LISTS ZLINK_CPP_SAMPLE_SOURCES)
    get_filename_component(sample_name "${sample_source}" NAME_WE)
    add_cpp_sample(
      "sample_cpp_${sample_name}"
      "samples/${sample_source}"
    )
  endforeach()
endif()

if(ZLINK_CPP_BUILD_TESTS)
  function(add_cpp_contract_test target_name source_file)
    add_cpp_binding_executable(${target_name} ${source_file})
    if(ZLINK_CPP_REGISTER_CTEST)
      if(UNIX AND NOT WIN32)
        add_test(NAME ${target_name}
          COMMAND /bin/bash -lc "$<TARGET_FILE:${target_name}>")
      else()
        add_test(NAME ${target_name} COMMAND ${target_name})
      endif()
      set_tests_properties(${target_name} PROPERTIES
        LABELS "contract"
        TIMEOUT 120
      )
      configure_cpp_binding_runtime(${target_name})
    endif()
  endfunction()

  set(ZLINK_CPP_CONTRACT_TEST_SOURCES
    test_cpp_contract_message.cpp
    test_cpp_contract_socket.cpp
    test_cpp_contract_request_reply.cpp
    test_cpp_contract_options.cpp
    test_cpp_contract_monitor.cpp
    test_cpp_contract_raw_capability.cpp
    test_cpp_contract_layout_headers.cpp
    test_cpp_contract_source_layout.cpp
    test_cpp_contract_behavior.cpp
    test_cpp_contract_optimization_guard.cpp
  )
  foreach(test_source IN LISTS ZLINK_CPP_CONTRACT_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_cpp_contract_test(
      "${test_name}"
      "tests/contract/${test_source}"
    )
  endforeach()
  target_include_directories(test_cpp_contract_request_reply PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
  )

  add_executable(
    test_cpp_contract_common_header_version
    tests/contract/test_cpp_contract_common_header_version.cpp
  )
  target_include_directories(test_cpp_contract_common_header_version
    BEFORE PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/include
  )
  target_compile_features(test_cpp_contract_common_header_version PRIVATE cxx_std_20)
  set_target_properties(test_cpp_contract_common_header_version PROPERTIES
    CXX_EXTENSIONS OFF
  )
  if(ZLINK_CPP_REGISTER_CTEST)
    if(UNIX AND NOT WIN32)
      add_test(NAME test_cpp_contract_common_header_version
        COMMAND /bin/bash -lc "$<TARGET_FILE:test_cpp_contract_common_header_version>")
    else()
      add_test(NAME test_cpp_contract_common_header_version COMMAND test_cpp_contract_common_header_version)
    endif()
    set_tests_properties(test_cpp_contract_common_header_version PROPERTIES
      LABELS "contract"
      TIMEOUT 120
      WORKING_DIRECTORY ${ZLINK_REPO_ROOT_DIR}
    )
  endif()

endif()

option(ZLINK_CPP_BUILD_BENCHMARKS "Build C++ binding benchmarks" OFF)

if(ZLINK_CPP_BUILD_BENCHMARKS)
  add_subdirectory(perf)
endif()
