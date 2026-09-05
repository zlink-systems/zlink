# CMake build script for Zlink tests
cmake_minimum_required(VERSION 3.10)

# On Windows: solution file will be called tests.sln
project(tests)

set(TEST_PTY_WRAPPER "${CMAKE_CURRENT_LIST_DIR}/run_under_pty.sh")
set(REGISTERED_TESTS)
set(SPLIT_ONLY_TEST_TARGETS
    test_backpressure_matrix
    test_backpressure_oneway_matrix
    test_disconnect_progress
    test_dealer_router_single_lane_contract)
set(INTEGRATION_TEST_ROOT "integration")
set(E2E_TEST_ROOT "e2e")

function(zlink_warn_unregistered_tests root_dir glob_pattern)
  set(options SKIP_FUZZERS)
  set(one_value_args LIST_FILE)
  set(multi_value_args ALLOWLIST)
  cmake_parse_arguments(ZLINK_TEST_SCAN "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT ZLINK_TEST_SCAN_LIST_FILE)
    set(ZLINK_TEST_SCAN_LIST_FILE "${CMAKE_CURRENT_LIST_FILE}")
  endif()

  file(READ "${ZLINK_TEST_SCAN_LIST_FILE}" CURRENT_LIST_FILE_CONTENT)
  file(GLOB_RECURSE ALL_TEST_SOURCES "${root_dir}/${glob_pattern}")

  foreach(TEST_SOURCE ${ALL_TEST_SOURCES})
    get_filename_component(TESTNAME "${TEST_SOURCE}" NAME_WE)

    if(ZLINK_TEST_SCAN_SKIP_FUZZERS AND TESTNAME MATCHES "_fuzzer$")
      continue()
    endif()

    list(FIND ZLINK_TEST_SCAN_ALLOWLIST "${TESTNAME}" TEST_ALLOWLIST_INDEX)
    if(TEST_ALLOWLIST_INDEX GREATER -1)
      continue()
    endif()

    string(REGEX MATCH "(^|[^A-Za-z0-9_])${TESTNAME}([^A-Za-z0-9_]|$)" MATCH_TESTNAME "${CURRENT_LIST_FILE_CONTENT}")
    if(NOT MATCH_TESTNAME)
      message(AUTHOR_WARNING "Test '${TESTNAME}' is not known to CTest.")
    endif()
  endforeach()
endfunction()

set(STREAM_SCENARIO_ROOT
    "${ZLINK_REPO_ROOT}/bindings/c/bench/with_stream/stacks")
if(NOT EXISTS "${STREAM_SCENARIO_ROOT}")
  set(STREAM_SCENARIO_ROOT
      "${ZLINK_CORE_SOURCE_DIR}/bench/with_stream/stacks")
endif()
if(NOT EXISTS "${STREAM_SCENARIO_ROOT}")
  set(STREAM_SCENARIO_ROOT
      "${ZLINK_CORE_SOURCE_DIR}/bench/benchwithstreamcompare/stacks")
endif()

option(
  ZLINK_CORE_BUILD_BINDING_INTEGRATION_TESTS
  "Build core tests that depend on bindings/c perf or bench sources"
  OFF)

if(ZLINK_CORE_BUILD_BINDING_INTEGRATION_TESTS
   AND EXISTS "${ZLINK_REPO_ROOT}/bindings/c/CMakeLists.txt")
  set(ZLINK_C_REGISTER_CTEST OFF CACHE BOOL
      "Do not let bindings/c register duplicate CTest entries from core/tests"
      FORCE)
  set(ZLINK_C_BUILD_BENCHMARKS ON CACHE BOOL
      "Build bindings/c perf helpers needed by core integration smoke tests"
      FORCE)
  set(ZLINK_C_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(ZLINK_C_BUILD_BENCHES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${ZLINK_REPO_ROOT}/bindings/c"
                   "${CMAKE_BINARY_DIR}/bindings-c")
endif()

set(tests
  ${INTEGRATION_TEST_ROOT}/test_request_writable_credit
  ${INTEGRATION_TEST_ROOT}/test_ancillaries
  ${INTEGRATION_TEST_ROOT}/test_asio_ws
  ${INTEGRATION_TEST_ROOT}/monitoring/test_monitor_enhanced
  ${INTEGRATION_TEST_ROOT}/monitoring/test_monitor_connection_identity
  ${INTEGRATION_TEST_ROOT}/monitoring/test_monitor_socket_contract
  ${INTEGRATION_TEST_ROOT}/monitoring/test_monitor_perf_contract
  ${INTEGRATION_TEST_ROOT}/monitoring/test_multi_stream_server_reassembly
  ${INTEGRATION_TEST_ROOT}/test_connect_resolve
  ${INTEGRATION_TEST_ROOT}/test_capabilities
  ${INTEGRATION_TEST_ROOT}/test_ctx_destroy
  ${INTEGRATION_TEST_ROOT}/test_ctx_term_fixed_rid_handover
  ${INTEGRATION_TEST_ROOT}/test_router_reciprocal_handover_lanes
  ${INTEGRATION_TEST_ROOT}/test_ctx_options
  ${INTEGRATION_TEST_ROOT}/test_socket_null
  ${INTEGRATION_TEST_ROOT}/test_multi_socket_contract_regressions
  ${INTEGRATION_TEST_ROOT}/test_gap_h3_pair_contract
  ${INTEGRATION_TEST_ROOT}/test_gap_h3_pubsub_contract
  ${INTEGRATION_TEST_ROOT}/test_backpressure_matrix
  ${INTEGRATION_TEST_ROOT}/test_backpressure_oneway_matrix
  ${INTEGRATION_TEST_ROOT}/test_public_inproc_multipart_send
  ${INTEGRATION_TEST_ROOT}/test_helper_send_part_basic
  ${INTEGRATION_TEST_ROOT}/test_helper_recv_part_basic
  ${INTEGRATION_TEST_ROOT}/test_helper_ownership
  ${INTEGRATION_TEST_ROOT}/test_helper_more_bad_send
  ${INTEGRATION_TEST_ROOT}/test_helper_interleave
  ${INTEGRATION_TEST_ROOT}/test_proxy
  ${INTEGRATION_TEST_ROOT}/test_pubsub
  ${INTEGRATION_TEST_ROOT}/test_reconnect_ivl
  ${INTEGRATION_TEST_ROOT}/test_reconnect_options
  ${INTEGRATION_TEST_ROOT}/test_socket_disconnect_boundary
  ${INTEGRATION_TEST_ROOT}/test_request_explicit_removal_not_found
  ${INTEGRATION_TEST_ROOT}/test_socket_disconnect_progress_without_app_poll
  ${INTEGRATION_TEST_ROOT}/test_connect_rid
  ${INTEGRATION_TEST_ROOT}/test_connect_rid_alias_binding
  ${INTEGRATION_TEST_ROOT}/test_pubsub_filter_xpub
  ${INTEGRATION_TEST_ROOT}/test_xpub_nodrop
  ${INTEGRATION_TEST_ROOT}/test_timer_poller
  ${INTEGRATION_TEST_ROOT}/test_polling_contract
  ${INTEGRATION_TEST_ROOT}/test_close_completion_poller_release
  ${INTEGRATION_TEST_ROOT}/test_disconnect_progress
  ${INTEGRATION_TEST_ROOT}/test_phase3_completion_contract
  ${INTEGRATION_TEST_ROOT}/test_phase3_request_reply_contract
  ${INTEGRATION_TEST_ROOT}/test_request_writable_contract
  ${INTEGRATION_TEST_ROOT}/test_dealer_router_single_lane_contract
  ${INTEGRATION_TEST_ROOT}/test_zmp_request_reply
  ${INTEGRATION_TEST_ROOT}/test_router_handover
  ${INTEGRATION_TEST_ROOT}/test_router_reject_duplicate
  ${INTEGRATION_TEST_ROOT}/test_router_reject_disconnected_without_app_recv
  ${INTEGRATION_TEST_ROOT}/test_inproc_pending_connect_rejected_at_attach
  ${INTEGRATION_TEST_ROOT}/test_router_mandatory
  ${INTEGRATION_TEST_ROOT}/test_router_mandatory_hwm
  ${INTEGRATION_TEST_ROOT}/test_router_multiple_dealers
  ${INTEGRATION_TEST_ROOT}/test_probe_router
  ${INTEGRATION_TEST_ROOT}/test_router_concurrent_routed_recv
  ${INTEGRATION_TEST_ROOT}/test_zmp_request_reply_receive_transaction
  ${INTEGRATION_TEST_ROOT}/test_flow_state_paired
  ${INTEGRATION_TEST_ROOT}/test_flow_state_c_api
  ${INTEGRATION_TEST_ROOT}/test_zmp_metadata
  ${INTEGRATION_TEST_ROOT}/test_send_async_tls_admit
  ${INTEGRATION_TEST_ROOT}/test_zmp_ws_wss
  ${INTEGRATION_TEST_ROOT}/test_stream_socket
  ${INTEGRATION_TEST_ROOT}/test_stream_threadsafe
  ${INTEGRATION_TEST_ROOT}/test_stream_send_blocking_wakeup
  ${INTEGRATION_TEST_ROOT}/test_stream_fastpath
  ${INTEGRATION_TEST_ROOT}/test_transport_matrix
  ${INTEGRATION_TEST_ROOT}/routing-id/test_router_auto_id_format
  ${INTEGRATION_TEST_ROOT}/routing-id/test_stream_routing_id_size
)

if(ZLINK_CORE_BUILD_BINDING_INTEGRATION_TESTS)
  list(APPEND tests
    ${INTEGRATION_TEST_ROOT}/monitoring/test_multi_dealer_dealer_benchmark_process
    ${INTEGRATION_TEST_ROOT}/monitoring/test_multi_pubsub_benchmark_process
    ${INTEGRATION_TEST_ROOT}/monitoring/test_multi_stream_benchmark_process
    ${INTEGRATION_TEST_ROOT}/monitoring/test_single_pair_benchmark_process
    ${STREAM_SCENARIO_ROOT}/client/test_scenario_stream_client
    ${STREAM_SCENARIO_ROOT}/zlink/test_scenario_stream_zlink
    ${STREAM_SCENARIO_ROOT}/zlink_packet/test_scenario_stream_zlink_packet
    ${STREAM_SCENARIO_ROOT}/asio/test_scenario_stream_asio)
endif()

set(ROUTER_ROUTER_ZLINK_CONNECT_TEST
    "scenario/router_router/zlink-connect/test_scenario_router_router_zlink_connect")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${ROUTER_ROUTER_ZLINK_CONNECT_TEST}.cpp")
  list(APPEND tests ${ROUTER_ROUTER_ZLINK_CONNECT_TEST})
endif()

set(STREAM_SCENARIO_ZMQ_DIST_ROOT_DEFAULT
    "${STREAM_SCENARIO_ROOT}/zmq/libzmq_dist")

if(NOT DEFINED STREAM_SCENARIO_ZMQ_DIST_ROOT)
  set(
    STREAM_SCENARIO_ZMQ_DIST_ROOT
    "${STREAM_SCENARIO_ZMQ_DIST_ROOT_DEFAULT}"
    CACHE PATH "Bundled libzmq distribution root for stream scenario tests")
elseif(NOT EXISTS "${STREAM_SCENARIO_ZMQ_DIST_ROOT}"
       AND EXISTS "${STREAM_SCENARIO_ZMQ_DIST_ROOT_DEFAULT}")
  set(
    STREAM_SCENARIO_ZMQ_DIST_ROOT
    "${STREAM_SCENARIO_ZMQ_DIST_ROOT_DEFAULT}"
    CACHE PATH "Bundled libzmq distribution root for stream scenario tests"
          FORCE)
endif()

set(STREAM_SCENARIO_ZMQ_ARCH "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64|x64)$")
  set(STREAM_SCENARIO_ZMQ_ARCH "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|AARCH64|arm64|ARM64)$")
  set(STREAM_SCENARIO_ZMQ_ARCH "arm64")
endif()

set(STREAM_SCENARIO_ZMQ_PLATFORM "")
if(WIN32)
  set(STREAM_SCENARIO_ZMQ_PLATFORM "windows")
elseif(APPLE)
  set(STREAM_SCENARIO_ZMQ_PLATFORM "macos")
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(STREAM_SCENARIO_ZMQ_PLATFORM "linux")
endif()

set(STREAM_SCENARIO_ZMQ_ROOT "")
if(STREAM_SCENARIO_ZMQ_PLATFORM AND STREAM_SCENARIO_ZMQ_ARCH)
  set(
    STREAM_SCENARIO_ZMQ_ROOT
    "${STREAM_SCENARIO_ZMQ_DIST_ROOT}/${STREAM_SCENARIO_ZMQ_PLATFORM}-${STREAM_SCENARIO_ZMQ_ARCH}")
endif()

set(STREAM_SCENARIO_ZMQ_LIBRARY "")
set(STREAM_SCENARIO_ZMQ_RUNTIME_DIR "")
set(STREAM_SCENARIO_ZMQ_INCLUDE_DIR "")
if(STREAM_SCENARIO_ZMQ_ROOT)
  if(WIN32)
    file(
      GLOB
      STREAM_SCENARIO_ZMQ_LIBRARY
      "${STREAM_SCENARIO_ZMQ_ROOT}/lib/*.lib")
    set(STREAM_SCENARIO_ZMQ_RUNTIME_DIR "${STREAM_SCENARIO_ZMQ_ROOT}/bin")
  else()
    file(
      GLOB
      STREAM_SCENARIO_ZMQ_LIBRARY
      "${STREAM_SCENARIO_ZMQ_ROOT}/lib/libzmq.so"
      "${STREAM_SCENARIO_ZMQ_ROOT}/lib/libzmq.so.*"
      "${STREAM_SCENARIO_ZMQ_ROOT}/lib/libzmq.dylib")
    set(STREAM_SCENARIO_ZMQ_RUNTIME_DIR "${STREAM_SCENARIO_ZMQ_ROOT}/lib")
  endif()
  set(STREAM_SCENARIO_ZMQ_INCLUDE_DIR "${STREAM_SCENARIO_ZMQ_ROOT}/include")
endif()

set(STREAM_SCENARIO_ZMQ_LIBRARY_FILE "")
if(STREAM_SCENARIO_ZMQ_LIBRARY)
  list(GET STREAM_SCENARIO_ZMQ_LIBRARY 0 STREAM_SCENARIO_ZMQ_LIBRARY_FILE)
endif()

if(ZLINK_CORE_BUILD_BINDING_INTEGRATION_TESTS
   AND EXISTS "${STREAM_SCENARIO_ZMQ_INCLUDE_DIR}/zmq.h"
   AND STREAM_SCENARIO_ZMQ_LIBRARY_FILE)
  list(APPEND tests ${STREAM_SCENARIO_ROOT}/zmq/test_scenario_stream_zmq)
else()
  message(
    STATUS
      "stream scenario zmq test disabled: libzmq bundle not found under ${STREAM_SCENARIO_ZMQ_ROOT}")
endif()

option(ENABLE_CAPSH "Run tests that require sudo and capsh (for cap_net_admin)" OFF)

if(ENABLE_CAPSH)
  find_program(CAPSH_PROGRAM NAMES capsh)

  if(CAPSH_PROGRAM)
    list(APPEND tests ${INTEGRATION_TEST_ROOT}/test_pair_tcp_cap_net_admin)
  else()
    message(STATUS "capsh not found, skipping tests that require CAP_NET_ADMIN")
  endif()
endif()

# add location of platform.hpp for Windows builds
if(WIN32)
  add_definitions(-DZLINK_CUSTOM_PLATFORM_HPP)
  add_definitions(-D_WINSOCK_DEPRECATED_NO_WARNINGS)

  # Same name on 64bit systems
  link_libraries(ws2_32.lib)
endif()

add_library(
  unity STATIC
  "${ZLINK_CORE_SOURCE_DIR}/external/unity/unity.c" "${ZLINK_CORE_SOURCE_DIR}/external/unity/unity.h"
  "${ZLINK_CORE_SOURCE_DIR}/external/unity/unity_internals.h")
set_target_properties(unity PROPERTIES PUBLIC_HEADER "${ZLINK_CORE_SOURCE_DIR}/external/unity/unity.h")
target_compile_definitions(unity PUBLIC "UNITY_USE_COMMAND_LINE_ARGS" "UNITY_EXCLUDE_FLOAT")
target_include_directories(unity PUBLIC "${ZLINK_CORE_SOURCE_DIR}/external/unity")

set(TESTUTIL_SOURCES
  testutil.cpp
  testutil.hpp
  testutil_monitoring.cpp
  testutil_monitoring.hpp
  testutil_unity.cpp
  testutil_unity.hpp)

# Shared library builds export only the public zlink_* C API (Windows DLL
# exports and the Linux version script agree on this), so Core tests link the
# static library to reach private helper symbols without expanding the ABI.
add_library(testutil-static STATIC ${TESTUTIL_SOURCES})
target_link_libraries(
  testutil-static
  libzlink-static ${OPTIONAL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} unity)
set(TESTUTIL_LIB testutil-static)

add_subdirectory(unittest)

if(MSVC_VERSION LESS 1700)
  set_source_files_properties("${ZLINK_CORE_SOURCE_DIR}/external/unity/unity.c" PROPERTIES LANGUAGE CXX)
endif()

if(MSVC_VERSION LESS 1600)
  target_compile_definitions(unity PUBLIC "UNITY_EXCLUDE_STDINT_H")
endif()

# add include dirs for all targets
include_directories("${ZLINK_CORE_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}")
include_directories("${CMAKE_CURRENT_LIST_DIR}")

# Add Boost include directory for ASIO tests (ASIO is mandatory)
include_directories("${BUNDLED_BOOST_INCLUDE_DIR}")

if(WIN32)
  add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
endif()

# Does not work, times out every time
if(WIN32)
  list(REMOVE_ITEM tests ${INTEGRATION_TEST_ROOT}/test_many_sockets)
endif()

foreach(test_source ${tests})
  get_filename_component(test ${test_source} NAME)
  # target_sources not supported before CMake 3.1
  add_executable(${test} ${test_source}.cpp)

  target_link_libraries(${test} ${TESTUTIL_LIB})

  if(WIN32)
    # This is the output for Debug dynamic builds on Visual Studio 6.0 You should provide the correct directory, don't
    # know how to do it automatically
    find_path(LIBZLINK_PATH "libzlink.lib" PATHS "../bin/Win32/Debug/v120/dynamic")

    if(NOT ${LIBZLINK_PATH} STREQUAL "LIBZLINK_PATH-NOTFOUND")
      set_target_properties(${test} PROPERTIES LINK_FLAGS "/LIBPATH:${LIBZLINK_PATH}")
    endif()
  else()
    # per-test directories not generated on OS X / Darwin
    if(NOT APPLE)
      set_property(
        TARGET ${test} APPEND PROPERTY LINK_DIRECTORIES
        "${Zlink_SOURCE_DIR}/../lib")
    endif()
  endif()

  if(RT_LIBRARY)
    target_link_libraries(${test} ${RT_LIBRARY})
  endif()

  if(CMAKE_SYSTEM_NAME MATCHES "QNX")
    target_link_libraries(${test} socket)
    target_link_libraries(${test} m)
  endif()

  if(NOT test IN_LIST SPLIT_ONLY_TEST_TARGETS)
    list(APPEND REGISTERED_TESTS ${test})

    if(WIN32)
      add_test(
        NAME ${test}
        WORKING_DIRECTORY ${LIBRARY_OUTPUT_PATH}
        COMMAND ${test})
    else()
      if(${test} MATCHES "_cap_net_admin")
        add_test(NAME ${test} COMMAND sh -c "sudo ${CAPSH_PROGRAM} --caps=cap_net_admin+eip -- -c $<TARGET_FILE:${test}>")
      else()
        add_test(NAME ${test} COMMAND ${test})
      endif()
    endif()

    set_tests_properties(${test} PROPERTIES TIMEOUT 10)
    if(${test} STREQUAL "test_router_handover")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 30)
    endif()
    if(${test} STREQUAL "test_monitor_connection_identity")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 30)
    endif()
    if(${test} STREQUAL "test_router_reciprocal_handover_lanes")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 60)
    endif()
    if(${test} STREQUAL "test_ctx_term_fixed_rid_handover")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 30)
    endif()
    if(${test} STREQUAL "test_helper_request_sequence_failure")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 120)
    endif()
    if(${test} STREQUAL "test_zmp_request_reply_receive_transaction")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 30)
    endif()
    if(${test} STREQUAL "test_proxy")
      set_tests_properties(
        ${test}
        PROPERTIES
          TIMEOUT 30)
    endif()
    if(${test} STREQUAL "test_multi_stream_benchmark_process")
      set_tests_properties(${test} PROPERTIES TIMEOUT 120)
    endif()
    set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)

    # Automatically set library path for tests to find libzlink shared library
    if(WIN32)
      set_tests_properties(${test} PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:libzlink>;$ENV{PATH}")
    elseif(APPLE)
      set_tests_properties(${test} PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:libzlink>:$ENV{DYLD_LIBRARY_PATH}")
    else()
      set_tests_properties(${test} PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:libzlink>:$ENV{LD_LIBRARY_PATH}")
    endif()
  endif()

  if(QNX)
    install(TARGETS ${test} RUNTIME DESTINATION bin/)
  endif()
endforeach()

# Keep the monitor teardown reproducer in C so it remains a minimal public-API
# regression test. The source intentionally exits after the reaper has had a
# chance to destroy the PUB; the monitor handle and context are owned by that
# process and need not be reused after the source socket closes.
add_executable(
  test_pub_monitor_close_full_mask
  ${INTEGRATION_TEST_ROOT}/test_pub_monitor_close_full_mask.c)
set_target_properties(test_pub_monitor_close_full_mask PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(
  test_pub_monitor_close_full_mask
  libzlink-static ${OPTIONAL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_pub_monitor_close_full_mask COMMAND test_pub_monitor_close_full_mask)
set_tests_properties(test_pub_monitor_close_full_mask PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
if(WIN32)
  set_tests_properties(
    test_pub_monitor_close_full_mask
    PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:libzlink>;$ENV{PATH}")
elseif(APPLE)
  set_tests_properties(
    test_pub_monitor_close_full_mask
    PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:libzlink>:$ENV{DYLD_LIBRARY_PATH}")
elseif(BUILD_SHARED)
  set_tests_properties(
    test_pub_monitor_close_full_mask
    PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:libzlink>:$ENV{LD_LIBRARY_PATH}")
endif()

if(TARGET test_scenario_stream_client)
  target_sources(
    test_scenario_stream_client
    PRIVATE ${STREAM_SCENARIO_ROOT}/client/stream_client_core.cpp)
endif()

if(TARGET zlink_c)
  if(TARGET test_scenario_stream_zlink)
    target_link_libraries(test_scenario_stream_zlink zlink_c)
  endif()
  if(TARGET test_scenario_stream_zlink_packet)
    target_link_libraries(test_scenario_stream_zlink_packet zlink_c)
  endif()
endif()

if(TARGET test_scenario_stream_zmq)
  target_include_directories(
    test_scenario_stream_zmq PRIVATE ${STREAM_SCENARIO_ZMQ_INCLUDE_DIR})
  target_link_libraries(test_scenario_stream_zmq
                        ${STREAM_SCENARIO_ZMQ_LIBRARY_FILE})

  if(WIN32)
    set_tests_properties(
      test_scenario_stream_zmq
      PROPERTIES ENVIRONMENT
                 "PATH=${STREAM_SCENARIO_ZMQ_RUNTIME_DIR};$<TARGET_FILE_DIR:libzlink>;$ENV{PATH}")
  elseif(APPLE)
    set_tests_properties(
      test_scenario_stream_zmq
      PROPERTIES ENVIRONMENT
                 "DYLD_LIBRARY_PATH=${STREAM_SCENARIO_ZMQ_RUNTIME_DIR}:$<TARGET_FILE_DIR:libzlink>:$ENV{DYLD_LIBRARY_PATH}")
  else()
    set_tests_properties(
      test_scenario_stream_zmq
      PROPERTIES ENVIRONMENT
                 "LD_LIBRARY_PATH=${STREAM_SCENARIO_ZMQ_RUNTIME_DIR}:$<TARGET_FILE_DIR:libzlink>:$ENV{LD_LIBRARY_PATH}")
  endif()
endif()

# override timeout for these tests
if(TARGET test_reconnect_ivl)
  set_tests_properties(test_reconnect_ivl PROPERTIES TIMEOUT 15)
endif()
set_tests_properties(test_socket_disconnect_boundary PROPERTIES TIMEOUT 60)
# Four connect-before-bind admission cases, each with monitor-driven waits.
set_tests_properties(test_inproc_pending_connect_rejected_at_attach PROPERTIES TIMEOUT 60)
# Twelve cases each observe past the original one-second request deadline.
set_tests_properties(test_request_explicit_removal_not_found PROPERTIES TIMEOUT 30)
if(TARGET test_socket_disconnect_progress_without_app_poll)
  set_tests_properties(
    test_socket_disconnect_progress_without_app_poll
    PROPERTIES TIMEOUT 60)
endif()
if(TARGET test_issue_566)
  set_tests_properties(test_issue_566 PROPERTIES TIMEOUT 30)
endif()
if(TARGET test_transport_matrix)
  # ROUTER/ROUTER uses two physical connections, so TLS/WSS matrix cases
  # still perform twice as many transport handshakes as single-lane D/R.
set_tests_properties(test_transport_matrix PROPERTIES TIMEOUT 120)
set_tests_properties(test_router_concurrent_routed_recv PROPERTIES TIMEOUT 120)
set_tests_properties(test_zmp_metadata PROPERTIES TIMEOUT 30)
endif()
if(TARGET test_phase3_request_reply_contract)
  set_tests_properties(
    test_phase3_request_reply_contract
    PROPERTIES TIMEOUT 180)
endif()
if(TARGET test_zmp_request_reply)
  set_tests_properties(
    test_zmp_request_reply
    PROPERTIES
      TIMEOUT 90
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_send_async_tls_admit)
  set_tests_properties(
    test_send_async_tls_admit
    PROPERTIES
      TIMEOUT 90
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_zmp_ws_wss)
  # The target covers WS and, when enabled, two additional TLS/WSS handshakes.
  set_tests_properties(test_zmp_ws_wss PROPERTIES TIMEOUT 60)
endif()
if(TARGET test_xpub_nodrop)
  set_tests_properties(test_xpub_nodrop PROPERTIES TIMEOUT 45)
endif()

foreach(h3_contract_test
        test_gap_h3_pubsub_contract
        test_flow_state_c_api)
  if(TARGET ${h3_contract_test})
    set_tests_properties(${h3_contract_test} PROPERTIES TIMEOUT 30)
  endif()
endforeach()
if(TARGET test_monitor_socket_contract)
  set_tests_properties(
    test_monitor_socket_contract
    PROPERTIES
      TIMEOUT 120
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_monitor_perf_contract)
  set_tests_properties(test_monitor_perf_contract PROPERTIES TIMEOUT 30)
endif()
if(TARGET test_helper_request_sequence_failure)
  set_tests_properties(
    test_helper_request_sequence_failure
    PROPERTIES
      TIMEOUT 120)
endif()
if(TARGET test_multi_pubsub_benchmark_process)
  if(TARGET comp_src_pubsub_server)
    add_dependencies(
      test_multi_pubsub_benchmark_process
      comp_src_pubsub_server)
  endif()
  if(TARGET comp_src_pubsub_client)
    add_dependencies(
      test_multi_pubsub_benchmark_process
      comp_src_pubsub_client)
  endif()
  set_tests_properties(
    test_multi_pubsub_benchmark_process
    PROPERTIES
      TIMEOUT 60
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_multi_dealer_dealer_benchmark_process)
  if(TARGET comp_src_dealer_dealer_server)
    add_dependencies(
      test_multi_dealer_dealer_benchmark_process
      comp_src_dealer_dealer_server)
  endif()
  if(TARGET comp_src_dealer_dealer_client)
    add_dependencies(
      test_multi_dealer_dealer_benchmark_process
      comp_src_dealer_dealer_client)
  endif()
  set_tests_properties(
    test_multi_dealer_dealer_benchmark_process
    PROPERTIES
      TIMEOUT 180
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_single_pair_benchmark_process)
  if(TARGET perf_pair)
    add_dependencies(
      test_single_pair_benchmark_process
      perf_pair)
  endif()
  set_tests_properties(
    test_single_pair_benchmark_process
    PROPERTIES
      TIMEOUT 30
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
endif()
if(TARGET test_stream_socket)
  set_tests_properties(test_stream_socket PROPERTIES TIMEOUT 60)
endif()
if(TARGET test_stream_fastpath)
  set_tests_properties(test_stream_fastpath PROPERTIES TIMEOUT 30)
endif()
function(add_env_case_test test_name target_name test_case)
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env
            ZLINK_TEST_CASE=${test_case}
            $<TARGET_FILE:${target_name}>)
endfunction()

foreach(transport tcp inproc)
  foreach(mask in completion)
    foreach(operation automatic disconnect request)
      foreach(wait idle blocked)
        set(case "${transport}_${mask}_${operation}_${wait}")
        set(name "test_disconnect_progress_${case}")
        add_env_case_test(${name} test_disconnect_progress ${case})
        set_tests_properties(${name} PROPERTIES
          LABELS "integration;serial"
          RUN_SERIAL TRUE
          TIMEOUT 10)
      endforeach()
    endforeach()
  endforeach()
endforeach()

function(extract_named_unity_cases out_var source_file macro_name)
  file(
    STRINGS "${source_file}" case_lines
    REGEX "^[ \t]*${macro_name} \\([A-Za-z0-9_]+\\);")
  set(cases)
  foreach(line IN LISTS case_lines)
    string(
      REGEX REPLACE
      "^[ \t]*${macro_name} \\(([A-Za-z0-9_]+)\\);.*$"
      "\\1"
      case_name
      "${line}")
    list(APPEND cases "${case_name}")
  endforeach()
  set(${out_var} "${cases}" PARENT_SCOPE)
endfunction()

function(assert_named_case_coverage label source_file macro_name expected_cases_var)
  extract_named_unity_cases(actual_cases "${source_file}" "${macro_name}")
  set(expected_cases "${${expected_cases_var}}")
  if(NOT "${actual_cases}" STREQUAL "${expected_cases}")
    message(
      FATAL_ERROR
      "${label} split case coverage mismatch.\n"
      "Source file: ${source_file}\n"
      "Expected cases: ${expected_cases}\n"
      "Actual cases: ${actual_cases}")
  endif()
endfunction()

function(add_env_case_tests_from_specs target_name specs_var)
  foreach(spec IN LISTS ${specs_var})
    string(REPLACE "=" ";" parts "${spec}")
    list(LENGTH parts part_count)
    if(NOT part_count EQUAL 2)
      message(FATAL_ERROR "Invalid case spec '${spec}'")
    endif()
    list(GET parts 0 test_name)
    list(GET parts 1 test_case)
    add_env_case_test(${test_name} ${target_name} ${test_case})
  endforeach()
endfunction()

function(add_env_case_suite target_name specs_var out_names_var)
  add_env_case_tests_from_specs(${target_name} ${specs_var})
  extract_test_names_from_specs(test_names ${specs_var})
  set(${out_names_var} "${test_names}" PARENT_SCOPE)
endfunction()

function(set_zlink_network_case_properties tests_var timeout)
  set(options ALLOW_SKIP)
  set(one_value_args WORKING_DIRECTORY)
  cmake_parse_arguments(
    ZLINK_NETWORK_CASE
    "${options}"
    "${one_value_args}"
    ""
    ${ARGN})

  set(test_names ${${tests_var}})
  if(NOT test_names)
    return()
  endif()

  set(properties
      TIMEOUT ${timeout}
      RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")
  if(ZLINK_NETWORK_CASE_ALLOW_SKIP)
    list(APPEND properties SKIP_RETURN_CODE 77)
  endif()
  if(ZLINK_NETWORK_CASE_WORKING_DIRECTORY)
    list(APPEND properties
         WORKING_DIRECTORY "${ZLINK_NETWORK_CASE_WORKING_DIRECTORY}")
  endif()

  set_tests_properties(${test_names} PROPERTIES ${properties})
endfunction()

function(set_zlink_scenario_properties target_name timeout labels)
  if(TARGET ${target_name})
    set_tests_properties(${target_name} PROPERTIES
      TIMEOUT ${timeout}
      LABELS "${labels}")
  endif()
endfunction()

function(extract_spec_fields out_var specs_var field_index)
  set(values)
  foreach(spec IN LISTS ${specs_var})
    string(REPLACE "=" ";" parts "${spec}")
    list(LENGTH parts part_count)
    if(NOT part_count EQUAL 2)
      message(FATAL_ERROR "Invalid case spec '${spec}'")
    endif()
    list(GET parts ${field_index} value)
    list(APPEND values "${value}")
  endforeach()
  set(${out_var} "${values}" PARENT_SCOPE)
endfunction()

function(extract_test_names_from_specs out_var specs_var)
  extract_spec_fields(test_names ${specs_var} 0)
  set(${out_var} "${test_names}" PARENT_SCOPE)
endfunction()

function(extract_case_names_from_specs out_var specs_var)
  extract_spec_fields(case_names ${specs_var} 1)
  set(${out_var} "${case_names}" PARENT_SCOPE)
endfunction()

set(NETWORK_INTEGRATION_RESOURCE_LOCK "zlink-network-integration")
set(STREAM_SOCKET_REGRESSION_TEST_SPECS
    test_stream_socket_phase3_mode_freeze=test_stream_phase3_mode_freeze_contract
    test_stream_socket_phase3_packet_pull=test_stream_phase3_packet_pull_contract
    test_stream_socket_phase3_packet_maxmsgsize=test_stream_phase3_packet_maxmsgsize_contract
    test_stream_socket_phase3_packet_enqueue_close_race=test_stream_phase3_packet_enqueue_close_race
    test_stream_socket_recv_multiclient_ready_regression=test_stream_recv_multiclient_strict_ready_gating_regression)

set(STREAM_THREADSAFE_REGRESSION_TEST_SPECS
    test_stream_threadsafe_send_part=test_stream_send_part_rid_is_thread_safe
    test_stream_threadsafe_high_contention=test_stream_send_part_rid_high_contention_is_thread_safe
    test_stream_threadsafe_endpoint_reads=test_stream_last_endpoint_read_is_safe_during_send
    test_stream_threadsafe_event_reads=test_stream_events_read_is_safe_during_send
    test_stream_threadsafe_runtime_reads=test_stream_runtime_reads_are_safe_during_send)

set(REQUEST_REPLY_RECOVERY_TEST_SPECS
    test_request_reply_recovery_tcp=test_request_reply_over_tcp_with_explicit_routing_id
    test_request_reply_recovery_tls=test_request_reply_over_tls_with_explicit_routing_id
    test_request_reply_recovery_out_of_order=test_out_of_order_replies_match_completion_ids)

set(TLS_ADMISSION_RECOVERY_TEST_SPECS
    test_tls_admission_during_handshake=test_writable_token_retries_over_tls_during_handshake)

set(BACKPRESSURE_MATRIX_INTEGRATION_TEST_SPECS
    test_backpressure_matrix_single_socket=test_single_socket_backpressure_matrix)

set(BACKPRESSURE_ONEWAY_INTEGRATION_TEST_SPECS
    test_backpressure_oneway_matrix_single_socket=test_single_socket_backpressure_oneway_matrix)

set(BACKPRESSURE_MATRIX_REGRESSION_TEST_SPECS
    test_backpressure_matrix_single_socket_regression=test_single_socket_backpressure_matrix
    test_backpressure_matrix_pubsub_regression=test_multi_pubsub_backpressure_matrix)

set(BACKPRESSURE_ONEWAY_REGRESSION_TEST_SPECS
    test_backpressure_oneway_matrix_single_socket_regression=test_single_socket_backpressure_oneway_matrix
    test_backpressure_oneway_matrix_pubsub_regression=test_multi_pubsub_backpressure_oneway_matrix)

set(H3_FLOW_CONTRACT_TEST_SPECS
    test_gap_h3_flow_same_state=same-state
    test_gap_h3_flow_resumed_stale=resumed-stale
    test_gap_h3_flow_hwm_resumed=hwm-resumed
    test_gap_h3_flow_event_mask=mask
    test_gap_h3_flow_ordering=ordering)

set(DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_SPECS
    test_single_lane_wire_dr_count_one=test_sl_wire_dr_count_one_ready_metadata
    test_single_lane_wire_rr_count_two=test_sl_wire_rr_count_two_ready_fence
    test_single_lane_wire_nonpaired_count_one=test_sl_wire_nonpaired_patterns_omit_lane_metadata
    test_single_lane_wire_symmetric_owner=test_sl_wire_symmetric_direction_and_inproc_order
    test_single_lane_wire_mandatory_count=test_sl_wire_mandatory_lane_count_rejections
    test_single_lane_wire_old_peer_rejected=test_sl_wire_old_peer_without_lane_count_rejected
    test_single_lane_wire_transport_matrix=test_sl_wire_transport_matrix_counts
    test_single_lane_request_reply_at_head=test_sl_request_reply_at_head_completion_only
    test_single_lane_request_data_before_reply=test_sl_request_multipart_data_before_reply_fifo
    test_single_lane_request_reply_before_data=test_sl_request_reply_before_data_destinations
    test_single_lane_request_timeout_wins=test_sl_request_timeout_wins_once_over_late_reply
    test_single_lane_request_reply_backpressure=test_sl_request_reply_submit_obeys_dr_backpressure
    test_single_lane_request_rr_isolation=test_sl_request_rr_reply_isolated_from_application_pause
    test_single_lane_request_type_restriction=test_sl_request_router_to_dealer_type_restriction
    test_single_lane_flow_dr_application_path=test_sl_flow_dr_application_control_is_not_public_data
    test_single_lane_flow_rr_completion_path=test_sl_flow_rr_completion_control_progresses_under_pause
    test_single_lane_flow_normal_kind_gate=test_sl_flow_dr_normal_kinds_share_pause_and_hwm_gate
    test_single_lane_flow_control_boundary=test_sl_flow_control_boundary_coalesces_latest_state
    test_single_lane_flow_stale_generation=test_sl_flow_stale_generation_is_fenced
    test_single_lane_flow_reconnect_resync=test_sl_flow_reconnect_resynchronizes_absolute_state
    test_single_lane_flow_snapshot_accounting=test_sl_flow_snapshot_accounts_dr_reply_as_application
    test_single_lane_monitor_lane_values=test_sl_monitor_lane_values_follow_topology
    test_single_lane_monitor_logical_ready=test_sl_monitor_logical_ready_once_per_peer
    test_single_lane_monitor_polling_split=test_sl_monitor_polling_separates_data_and_completion
    test_single_lane_monitor_detach_count_one=test_sl_monitor_detach_count_one_has_single_lifecycle
    test_single_lane_monitor_detach_count_two=test_sl_monitor_detach_count_two_retires_pair
    test_single_lane_monitor_reply_reconnect=test_sl_monitor_reply_token_reconnect_uses_current_application
    test_single_lane_monitor_status_abi=test_sl_monitor_status_abi_and_flow_detail_stable
    test_single_lane_monitor_status_mask=test_sl_monitor_status_ready_is_independent_of_event_mask)

if(TARGET test_stream_socket)
  add_env_case_suite(
    test_stream_socket
    STREAM_SOCKET_REGRESSION_TEST_SPECS
    STREAM_SOCKET_REGRESSION_TEST_NAMES)
  set_zlink_network_case_properties(
    STREAM_SOCKET_REGRESSION_TEST_NAMES
    60)
endif()
if(TARGET test_stream_threadsafe)
  add_env_case_suite(
    test_stream_threadsafe
    STREAM_THREADSAFE_REGRESSION_TEST_SPECS
    STREAM_THREADSAFE_REGRESSION_TEST_NAMES)
  set_zlink_network_case_properties(
    STREAM_THREADSAFE_REGRESSION_TEST_NAMES
    45)
endif()
if(TARGET test_zmp_request_reply)
  add_env_case_suite(
    test_zmp_request_reply
    REQUEST_REPLY_RECOVERY_TEST_SPECS
    REQUEST_REPLY_RECOVERY_TEST_NAMES)
  set_zlink_network_case_properties(
    REQUEST_REPLY_RECOVERY_TEST_NAMES
    90)
endif()
if(TARGET test_send_async_tls_admit)
  add_env_case_suite(
    test_send_async_tls_admit
    TLS_ADMISSION_RECOVERY_TEST_SPECS
    TLS_ADMISSION_RECOVERY_TEST_NAMES)
  set_zlink_network_case_properties(
    TLS_ADMISSION_RECOVERY_TEST_NAMES
    90)
endif()
if(TARGET test_backpressure_matrix)
  add_env_case_suite(
    test_backpressure_matrix
    BACKPRESSURE_MATRIX_INTEGRATION_TEST_SPECS
    BACKPRESSURE_MATRIX_INTEGRATION_TEST_NAMES)
  set_zlink_network_case_properties(
    BACKPRESSURE_MATRIX_INTEGRATION_TEST_NAMES
    120)
  add_env_case_suite(
    test_backpressure_matrix
    BACKPRESSURE_MATRIX_REGRESSION_TEST_SPECS
    BACKPRESSURE_MATRIX_REGRESSION_TEST_NAMES)
  set_zlink_network_case_properties(
    BACKPRESSURE_MATRIX_REGRESSION_TEST_NAMES
    180)
endif()
if(TARGET test_backpressure_oneway_matrix)
  add_env_case_suite(
    test_backpressure_oneway_matrix
    BACKPRESSURE_ONEWAY_INTEGRATION_TEST_SPECS
    BACKPRESSURE_ONEWAY_INTEGRATION_TEST_NAMES)
  set_zlink_network_case_properties(
    BACKPRESSURE_ONEWAY_INTEGRATION_TEST_NAMES
    120)
  add_env_case_suite(
    test_backpressure_oneway_matrix
    BACKPRESSURE_ONEWAY_REGRESSION_TEST_SPECS
    BACKPRESSURE_ONEWAY_REGRESSION_TEST_NAMES)
  set_zlink_network_case_properties(
    BACKPRESSURE_ONEWAY_REGRESSION_TEST_NAMES
    120)
endif()
if(TARGET test_flow_state_c_api)
  add_env_case_suite(
    test_flow_state_c_api
    H3_FLOW_CONTRACT_TEST_SPECS
    H3_FLOW_CONTRACT_TEST_NAMES)
  set_tests_properties(
    ${H3_FLOW_CONTRACT_TEST_NAMES}
    PROPERTIES TIMEOUT 15)
endif()
if(TARGET test_dealer_router_single_lane_contract)
  extract_case_names_from_specs(
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_CASES
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_SPECS)
  assert_named_case_coverage(
    "dealer-router single-lane contract"
    "${CMAKE_CURRENT_LIST_DIR}/${INTEGRATION_TEST_ROOT}/test_dealer_router_single_lane_contract.cpp"
    RUN_SINGLE_LANE_CONTRACT_TEST
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_CASES)
  add_env_case_suite(
    test_dealer_router_single_lane_contract
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_SPECS
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_NAMES)
  set_zlink_network_case_properties(
    DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_NAMES
    45)
  set_tests_properties(
    test_single_lane_wire_transport_matrix
    PROPERTIES TIMEOUT 180)
  set_tests_properties(
    test_single_lane_wire_mandatory_count
    test_single_lane_flow_reconnect_resync
    test_single_lane_monitor_detach_count_two
    PROPERTIES TIMEOUT 90)
endif()

set_zlink_scenario_properties(
  test_scenario_router_router_zlink_connect
  180
  "scenario;router_router;zlink_connect")
set_zlink_scenario_properties(test_scenario_stream_client 120 "scenario;stream;client")
set_zlink_scenario_properties(test_scenario_stream_zlink 120 "scenario;stream;zlink")
set_zlink_scenario_properties(
  test_scenario_stream_zlink_packet
  120
  "scenario;stream;zlink_packet")
set_zlink_scenario_properties(test_scenario_stream_asio 120 "scenario;stream;asio")
set_zlink_scenario_properties(test_scenario_stream_zmq 120 "scenario;stream;zmq")

set(E2E_SERIAL_TESTS
)
if(TARGET test_scenario_stream_client)
  list(APPEND E2E_SERIAL_TESTS test_scenario_stream_client)
endif()
if(TARGET test_scenario_stream_zlink)
  list(APPEND E2E_SERIAL_TESTS test_scenario_stream_zlink)
endif()
if(TARGET test_scenario_stream_zlink_packet)
  list(APPEND E2E_SERIAL_TESTS test_scenario_stream_zlink_packet)
endif()
if(TARGET test_scenario_stream_asio)
  list(APPEND E2E_SERIAL_TESTS test_scenario_stream_asio)
endif()
if(TARGET test_scenario_stream_zmq)
  list(APPEND E2E_SERIAL_TESTS test_scenario_stream_zmq)
endif()
if(TARGET test_scenario_router_router_zlink_connect)
  list(APPEND E2E_SERIAL_TESTS test_scenario_router_router_zlink_connect)
endif()

set(REGRESSION_SERIAL_TESTS
    ${BACKPRESSURE_MATRIX_REGRESSION_TEST_NAMES}
    ${BACKPRESSURE_ONEWAY_REGRESSION_TEST_NAMES}
    test_router_concurrent_routed_recv
    test_transport_matrix
    ${STREAM_SOCKET_REGRESSION_TEST_NAMES}
    ${STREAM_THREADSAFE_REGRESSION_TEST_NAMES}
    ${REQUEST_REPLY_RECOVERY_TEST_NAMES}
    ${TLS_ADMISSION_RECOVERY_TEST_NAMES})
if(TARGET test_multi_dealer_dealer_benchmark_process)
  list(APPEND REGRESSION_SERIAL_TESTS test_multi_dealer_dealer_benchmark_process)
endif()

set(INTEGRATION_SERIAL_TESTS ${REGISTERED_TESTS})
list(REMOVE_ITEM INTEGRATION_SERIAL_TESTS ${E2E_SERIAL_TESTS} ${REGRESSION_SERIAL_TESTS})
list(APPEND INTEGRATION_SERIAL_TESTS
     ${BACKPRESSURE_MATRIX_INTEGRATION_TEST_NAMES}
     ${BACKPRESSURE_ONEWAY_INTEGRATION_TEST_NAMES}
     ${H3_FLOW_CONTRACT_TEST_NAMES}
     ${DEALER_ROUTER_SINGLE_LANE_CONTRACT_TEST_NAMES})
list(REMOVE_ITEM INTEGRATION_SERIAL_TESTS ${E2E_SERIAL_TESTS} ${REGRESSION_SERIAL_TESTS})
list(REMOVE_DUPLICATES INTEGRATION_SERIAL_TESTS)

if(INTEGRATION_SERIAL_TESTS)
  set_tests_properties(${INTEGRATION_SERIAL_TESTS} PROPERTIES RUN_SERIAL TRUE)
  set_property(TEST ${INTEGRATION_SERIAL_TESTS} APPEND PROPERTY LABELS "integration;serial")
endif()

if(E2E_SERIAL_TESTS)
  set_tests_properties(${E2E_SERIAL_TESTS} PROPERTIES RUN_SERIAL TRUE)
  set_property(TEST ${E2E_SERIAL_TESTS} APPEND PROPERTY LABELS "e2e;serial")
endif()

if(REGRESSION_SERIAL_TESTS)
  set_tests_properties(${REGRESSION_SERIAL_TESTS} PROPERTIES
    RUN_SERIAL TRUE
    LABELS "regression;serial")
endif()

set(UNREGISTERED_TEST_ALLOWLIST
    test_asio_connect
    test_asio_poller
    test_asio_tcp
    test_bind_ws_fuzzer
    test_bind_after_connect_tcp
    test_bind_src_address
    test_connect_fuzzer
    test_connect_rid
    test_connect_rid_string_alias
    test_connect_null_fuzzer
    test_ctx_options
    test_diffserv
    test_issue_566
    test_immediate
    test_ipc_wildcard
    test_last_endpoint
    test_many_sockets
    test_disconnect_inproc
    test_asio_ssl
    test_abstract_ipc
    test_hwm_pubsub
    test_monitor_enhanced
    test_msg_ffn
    test_msg_flags
    test_pair_inproc
    test_pair_ipc
    test_pair_tcp
    test_pair_send_blocking_wakeup
    test_pub_invert_matching
    test_pubsub_inproc_alternating
    test_router_auto_id_format
    test_router_multiple_dealers
    test_reconnect_ivl
    test_reconnect_options
    test_socket_options_fuzzer
    test_spec_router
    test_stream_fastpath
    test_stream_socket
    test_sub_forward
    test_system
    test_timeo
    test_shutdown_stress
    test_xpub_manual
    test_xpub_nodrop
    test_xpub_topic
    test_xpub_verbose
    test_xpub_welcome_msg
    test_z85_decode_fuzzer)

zlink_warn_unregistered_tests(
  "${CMAKE_CURRENT_LIST_DIR}"
  "test_*.cpp"
  LIST_FILE "${CMAKE_CURRENT_LIST_FILE}"
  ALLOWLIST ${UNREGISTERED_TEST_ALLOWLIST}
  SKIP_FUZZERS)

# Public surface contract gate: header closure and dynamic exports must
# match the formal 10.0.0 spec exactly (see core/tests/contract/).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME contract_public_surface
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/contract/check_public_surface.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../..
      $<TARGET_FILE:libzlink>)
  set_tests_properties(contract_public_surface PROPERTIES
    TIMEOUT 60
    LABELS "unittest")
  add_test(
    NAME contract_c_header_mirror
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/contract/check_c_header_mirror.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  set_tests_properties(contract_c_header_mirror PROPERTIES
    TIMEOUT 30
    LABELS "unittest")
endif()

# Deterministic public-API instruction-count gate for the Core hot paths.
add_executable(hotpath_bench perf/hotpath_bench.cpp)
target_link_libraries(
  hotpath_bench
  libzlink-static ${OPTIONAL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
find_path(
  VALGRIND_INCLUDE_DIR
  valgrind/callgrind.h
  HINTS "$ENV{HOME}/.local/include")
if(VALGRIND_INCLUDE_DIR)
  target_include_directories(hotpath_bench PRIVATE "${VALGRIND_INCLUDE_DIR}")
endif()

find_program(
  VALGRIND_EXECUTABLE
  valgrind
  HINTS "$ENV{HOME}/.local/bin")
if(VALGRIND_EXECUTABLE AND Python3_Interpreter_FOUND)
  add_test(
    NAME hotpath_gate
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/perf/hotpath_gate.py
      --bench $<TARGET_FILE:hotpath_bench>
      --reference ${CMAKE_CURRENT_SOURCE_DIR}/perf/hotpath_reference.json
      --valgrind ${VALGRIND_EXECUTABLE})
  set_tests_properties(
    hotpath_gate
    PROPERTIES
      LABELS "hotpath"
      TIMEOUT 600)
endif()

# Level-triggered wake/lost-wake contract suite. Keep the two exact internal
# race reproducers in their owning executables and expose them as focused gates
# beside the public poll/poller matrix.
add_executable(
  test_wake_invariants
  ${INTEGRATION_TEST_ROOT}/test_wake_invariants.cpp)
target_link_libraries(test_wake_invariants ${TESTUTIL_LIB})

add_executable(
  test_two_poller_wake
  ${INTEGRATION_TEST_ROOT}/test_two_poller_wake.cpp)
target_link_libraries(test_two_poller_wake ${TESTUTIL_LIB})

# Regression: closing a SUB (linger 0) while its engine is still receiving a
# frame payload must not release the decoder frame reservation through the
# already-freed session (engine destruction is deferred past the session).
add_executable(
  test_pubsub_close_during_inbound_frame
  ${INTEGRATION_TEST_ROOT}/test_pubsub_close_during_inbound_frame.cpp)
target_link_libraries(test_pubsub_close_during_inbound_frame ${TESTUTIL_LIB})
add_test(NAME test_pubsub_close_during_inbound_frame
         COMMAND test_pubsub_close_during_inbound_frame)
set_tests_properties(
  test_pubsub_close_during_inbound_frame
  PROPERTIES
    LABELS "regression;serial"
    RUN_SERIAL TRUE
    TIMEOUT 180
    RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}")

add_test(NAME test_wake_invariants COMMAND test_wake_invariants)
add_test(NAME test_two_poller_wake COMMAND test_two_poller_wake)
add_test(
  NAME test_wake_invariant_hwm_lwm_shrink
  COMMAND test_router_multiple_dealers)
add_test(
  NAME test_wake_invariant_completion_owner
  COMMAND
    ${CMAKE_COMMAND} -E env
    ZLINK_TEST_CASE=test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle
    $<TARGET_FILE:test_phase3_request_reply_contract>)

set_tests_properties(
  test_wake_invariants
  test_two_poller_wake
  test_wake_invariant_hwm_lwm_shrink
  test_wake_invariant_completion_owner
  PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 180
    SKIP_RETURN_CODE 77
    RESOURCE_LOCK "${NETWORK_INTEGRATION_RESOURCE_LOCK}"
    LABELS "integration;regression;serial;wake-invariant")

set_tests_properties(test_two_poller_wake PROPERTIES TIMEOUT 60)

set_tests_properties(test_request_writable_credit PROPERTIES
  LABELS "integration;serial" RUN_SERIAL TRUE TIMEOUT 30)
