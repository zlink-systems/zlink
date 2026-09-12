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
    unittest_auto_hwm_incremental_plan
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

# This binary contains several independent socket contracts whose aggregate
# runtime exceeds the per-test CTest deadline on Windows. Keep the executable
# shared, but register each existing selector separately on Windows so other
# platforms retain their established aggregate gate.
if(WIN32)
  set(flow_state_socket_cases
    test_unsupported_socket_types_report_not_supported
    test_invalid_state_is_rejected
    test_remote_pause_blocks_sender_and_resume_releases_it
    test_local_hwm_and_remote_pause_are_independent
    test_pause_mid_multipart_preserves_atomicity
    test_duplicate_and_stale_frames_are_ignored
    test_new_and_reconnected_pairs_receive_the_latest_state
    test_flow_state_epoch_edge_cases
    test_generation_change_resets_the_epoch_sequence
    test_flow_frame_cannot_complete_a_truncated_reply
    test_epoch_zero_is_refused_by_the_pipe_command
    test_epoch_wraparound_forces_a_new_connection_generation
    test_flow_frame_before_registration_is_promoted_after_validation
    test_resume_rereads_credit_published_before_the_waiter_was_armed
    test_router_peer_state_reports_remote_pause
    test_flow_frame_before_reply_is_consumed_on_a_local_pair
    test_peer_weight_change_does_not_leak_to_public_receive
    test_inproc_peer_weight_is_owner_control_in_both_directions
    test_peer_weight_update_is_safe_for_async_readers
    test_pair_replacement_keeps_exact_peer_weight_state
    test_flow_frame_uses_count_selected_control_lane
    test_router_routing_id_part_holds_message_atomicity_across_pause
    test_resume_while_hwm_full_still_recovers_through_byte_credit
    test_stale_flow_state_command_cannot_override_a_newer_epoch
    test_no_application_recv_returns_a_flow_frame)

  # A misspelled or duplicated selector otherwise runs zero Unity cases and
  # exits successfully. Derive the authoritative sequence from the source and
  # fail configure unless the CTest registry covers it exactly once, in order.
  file(
    STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/unittest_flow_state_socket.cpp"
    flow_state_socket_case_lines
    REGEX "^[ \t]*RUN_FLOW_CASE \\([A-Za-z0-9_]+\\);")
  set(flow_state_socket_source_cases)
  foreach(case_line IN LISTS flow_state_socket_case_lines)
    string(
      REGEX REPLACE
      "^[ \t]*RUN_FLOW_CASE \\(([A-Za-z0-9_]+)\\);.*$"
      "\\1"
      case_name
      "${case_line}")
    list(APPEND flow_state_socket_source_cases "${case_name}")
  endforeach()
  if(NOT "${flow_state_socket_source_cases}" STREQUAL
         "${flow_state_socket_cases}")
    message(
      FATAL_ERROR
      "unittest_flow_state_socket split case coverage mismatch.\n"
      "Registered: ${flow_state_socket_cases}\n"
      "Source: ${flow_state_socket_source_cases}")
  endif()
endif()

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

  if(WIN32 AND test STREQUAL "unittest_flow_state_socket")
    set(registered_tests)
    foreach(test_case ${flow_state_socket_cases})
      set(test_name "${test}.${test_case}")
      add_test(
        NAME ${test_name}
        WORKING_DIRECTORY ${LIBRARY_OUTPUT_PATH}
        COMMAND ${test})
      set_tests_properties(
        ${test_name} PROPERTIES ENVIRONMENT "ZLINK_TEST_CASE=${test_case}")
      list(APPEND registered_tests ${test_name})
    endforeach()
  else()
    set(registered_tests ${test})
    if(WIN32)
      add_test(
        NAME ${test}
        WORKING_DIRECTORY ${LIBRARY_OUTPUT_PATH}
        COMMAND ${test})
    else()
      add_test(NAME ${test} COMMAND ${test})
    endif()
  endif()

  set_tests_properties(${registered_tests} PROPERTIES TIMEOUT 10)
  set_tests_properties(${registered_tests} PROPERTIES LABELS "unittest;parallel-safe")

endforeach()

zlink_warn_unregistered_tests(
  "${CMAKE_CURRENT_LIST_DIR}"
  "unittest_*.cpp"
  LIST_FILE "${CMAKE_CURRENT_LIST_FILE}")

# Applied last so that every deadline set above is scaled.
include("${CMAKE_CURRENT_LIST_DIR}/../scale_test_timeouts.cmake")
