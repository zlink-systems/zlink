# CMake build script for Zlink unit tests
cmake_minimum_required(VERSION 3.10)

set(unittests
    unittest_ypipe
    unittest_poller
    unittest_fast_mutex
    unittest_mtrie
    unittest_ip_resolver
    unittest_ipc_address
    unittest_radix_tree
    unittest_zmp_decoder
    unittest_raw_decoder
    unittest_msg_view
    unittest_ctx_runtime
    unittest_socket_runtime
    unittest_typed_option
    unittest_public_contract_headers
    unittest_request_completion_queue
    unittest_request_timeout_scheduler
    unittest_result_enum_mapping
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
    link_directories(${test} PRIVATE "${Zlink_SOURCE_DIR}/../lib")
  endif()

  target_link_libraries(${test} testutil-static)

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

  # Unit tests link through testutil-static and the static core library only.
  # Some CMake versions still propagate the shared libzlink target through
  # legacy link settings, so strip it after all per-test libraries are added.
  get_target_property(LIBS ${test} LINK_LIBRARIES)
  list(REMOVE_ITEM LIBS libzlink)
  set_target_properties(${test} PROPERTIES LINK_LIBRARIES "${LIBS}")
endforeach()

zlink_warn_unregistered_tests(
  "${CMAKE_CURRENT_LIST_DIR}"
  "unittest_*.cpp"
  LIST_FILE "${CMAKE_CURRENT_LIST_FILE}")
