# CMake build script for Zlink unit tests
cmake_minimum_required(VERSION 3.10)

include_directories(${CORE_TEST_INTERNAL_INCLUDE_DIRS})
add_compile_definitions(ZLINK_INTERNAL_BUILD=1 ZLINK_BUILD_TESTS=1)
# Internal class layouts must use the same configure probes as Core objects.
# Public helpers deliberately do not include this private configuration header.
get_target_property(CORE_TEST_BINARY_DIR test-core BINARY_DIR)
if(MSVC)
  add_compile_options("/FI${CORE_TEST_BINARY_DIR}/platform.hpp")
else()
  add_compile_options(-include "${CORE_TEST_BINARY_DIR}/platform.hpp")
endif()

set(unittests
    unittest_auto_hwm_physical_attempt
    unittest_zmp_engine_controls
    unittest_pubsub_raw_hwm
    unittest_ws_transport_config
    unittest_single_lane_accounting
    unittest_submit_errors
    unittest_submit_retry
    unittest_router_message_envelope
    unittest_complete_record_admission
    unittest_router_pipe_contract
    unittest_xsub_pipe_termination
    unittest_router_peer_weight
    unittest_receive_metadata
    unittest_publish_rollback
    unittest_monitor_ready_drain
    unittest_proxy_metadata
    unittest_monitor_queue_accounting
    unittest_zmp_pair_lifecycle
    unittest_ws_metadata
    unittest_phase3_request_reply_owners
    unittest_flow_state_socket
    unittest_flow_state_monitor
    unittest_ctx_lifecycle
    unittest_router_receive_queue
    unittest_receive_transaction
    unittest_ypipe
    unittest_poller
    unittest_socket_poller
    unittest_mutex
    unittest_mtrie
    unittest_ip_resolver
    unittest_ipc_address
    unittest_radix_tree
    unittest_zmp_decoder
    unittest_zmp_contract_edges
    unittest_raw_decoder
    unittest_msg_auxiliary
    unittest_msg_view
    unittest_ctx_runtime
    unittest_socket_runtime
    unittest_typed_option
    unittest_public_contract_headers
    unittest_socket_completion_queue
    unittest_request_timeout_scheduler
    unittest_result_enum_mapping
    unittest_auto_hwm_policy
    unittest_flow_state_frame
    unittest_pipe_byte_charge
    unittest_asio_write_turn_policy
    unittest_asio_transport_writev_lifetime
    unittest_control_runtime)

# add location of platform.hpp for Windows builds
if(WIN32)
  add_definitions(-DZLINK_CUSTOM_PLATFORM_HPP)
  add_definitions(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
  # Same name on 64bit systems
  link_libraries(ws2_32.lib)
endif()

include_directories("${ZLINK_CORE_SOURCE_DIR}/include"
                    "${ZLINK_CORE_SOURCE_DIR}/src"
                    "${CMAKE_BINARY_DIR}")
include_directories("${ZLINK_CORE_SOURCE_DIR}/external/unity")
# Add Boost headers for ASIO poller
include_directories("${BUNDLED_BOOST_INCLUDE_DIR}")

foreach(test ${unittests})
  # target_sources not supported before CMake 3.1
  add_executable(${test} ${test}.cpp "unittest_resolver_common.hpp")

  # per-test directories not generated on OS X / Darwin
  if(NOT ${CMAKE_CXX_COMPILER_ID} MATCHES "Clang.*")
    set_property(
      TARGET ${test} APPEND PROPERTY LINK_DIRECTORIES
      "${Zlink_SOURCE_DIR}/../lib")
  endif()

  target_link_libraries(${test} testutil-common test-core)
  set_property(TARGET ${test} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)

  if(RT_LIBRARY)
    target_link_libraries(${test} ${RT_LIBRARY})
  endif()

  if(CMAKE_SYSTEM_NAME MATCHES "QNX")
    target_link_libraries(${test} socket)
    target_link_libraries(${test} m)
  endif()

  if(WIN32)
    add_test(
      NAME ${test}
      WORKING_DIRECTORY ${LIBRARY_OUTPUT_PATH}
      COMMAND ${test})
  else()
    add_test(NAME ${test} COMMAND ${test})
  endif()

  set_tests_properties(${test} PROPERTIES TIMEOUT 10)
  set_tests_properties(${test} PROPERTIES LABELS "unittest;parallel-safe")

endforeach()

zlink_warn_unregistered_tests(
  "${CMAKE_CURRENT_LIST_DIR}"
  "unittest_*.cpp"
  LIST_FILE "${CMAKE_CURRENT_LIST_FILE}")
