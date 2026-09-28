if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_CONFIGURATION
    OR ZLINK_FRAMEWORK_CPP_CONFIGURATION STREQUAL "")
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_CONFIGURATION is required")
endif()

include("${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/tests/Zlink.Framework.ContractTests/release_test_labels.cmake")

set(required_labels
  framework-contract
  framework-unit
  framework-config
  framework-regression
  framework-host
  framework-integration
  framework-location
  framework-zlink
  framework-zlink-channel
  framework-zlink-spot
  framework-zlink-stream
  framework-zlink-actor-gateway
  framework-observability
  framework-http
  framework-http-e2e
  framework-perf-smoke
  framework-package
  framework-tooling
  http-client-contract
  http-client-unit
  http-client-e2e
  http-client-https
  http-client-regression
  connector-unit
  connector-integration
  connector-e2e
  connector-contract
  connector-coroutine
  connector-dispatch
  connector-protocol
  connector-transport
  connector-typed
  connector-timeout
  connector-package
  connector-regression
  connector-perf-smoke
  connector-perf-scale
  connector-unreal-contract
  connector-unreal-compile
  connector-unreal-smoke
  connector-godot-contract
  connector-godot-e2e
  connector-axmol-contract
  connector-axmol-e2e
  # test_cpp_framework_sample_parity is a contract test that always registers under
  # ZLINK_FRAMEWORK_CPP_BUILD_TESTS; unlike the sample programs themselves (built only under
  # ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES), its "framework-sample-parity" label is present
  # regardless of that flag, so it belongs here rather than in sample_labels below.
  framework-sample-parity)

set(sample_labels
  framework-sample-api
  framework-sample-bingo
  framework-sample-courier
  framework-sample-deliverydispatch
  framework-sample-dispatch
  framework-sample-gamequest
  framework-sample-gateway
  framework-sample-mission
  framework-sample-ops
  framework-sample-play
  framework-sample-registry
  framework-sample-session
  framework-sample-smoke
  framework-sample-shoppingmall
  framework-sample-support
  framework-sample-supportchat
  framework-sample-tictactoe
  framework-sample-tracking
  framework-sample-workflow
  framework-sample-zone-node
  framework-sample-zoneworld)

file(READ "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/CMakeCache.txt" build_cache)
if(build_cache MATCHES "ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT:BOOL=ON")
  list(APPEND required_labels framework-http-perf)
endif()
if(build_cache MATCHES "ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES:BOOL=ON")
  list(APPEND required_labels ${sample_labels})
endif()
# The engine adapter tests register only when their adapter is built.
if(build_cache MATCHES "ZLINK_STREAM_CONNECTOR_BUILD_GODOT:BOOL=ON")
  list(APPEND required_labels connector-godot-contract connector-godot-e2e)
endif()
if(build_cache MATCHES "ZLINK_STREAM_CONNECTOR_BUILD_AXMOL:BOOL=ON")
  list(APPEND required_labels connector-axmol-contract connector-axmol-e2e)
endif()

set(known_labels
  ${ZLINK_FRAMEWORK_CPP_TEST_TIERS}
  ${required_labels}
  ${sample_labels}
  framework-extension
  framework-client-server
  framework-monitoring
  parity
  ActorGateway
  CH-001
  CH-006
  DERR-001
  DERR-002
  DERR-006
  DERR-007
  DERR-009
  DI
  actor
  actor-join
  admission
  async
  backpressure
  channel
  claim
  diagnostics
  execution
  gtest
  handler
  hosted
  http
  messaging
  module
  monitoring
  metrics
  registry
  reliability
  redis
  runtime
  scope
  serializer
  spot
  spot-actor
  stream
  framework-actor
  # foundation and M6 runtime tests build headless of a service-layer framework and label
  # themselves with these finer-grained categories alongside the framework-unit/-contract tier.
  framework-foundation
  framework-m6-runtime
  instance-activation
  liveness
  mailbox
  maintenance
  operation
  protocol
  raw-binding
  recovery
  relocation
  reply
  resource
  stateful
  stream-session
  submit-admission
  termination
  topology
  yield)

if(ZLINK_FRAMEWORK_CPP_EXPECT_COVERAGE_LABEL)
  list(APPEND required_labels framework-coverage)
  list(APPEND known_labels framework-coverage)
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
    -C "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}" --print-labels
  RESULT_VARIABLE print_labels_result
  OUTPUT_VARIABLE print_labels_output
  ERROR_VARIABLE print_labels_error)
if(NOT print_labels_result EQUAL 0)
  message(FATAL_ERROR "ctest label print failed: ${print_labels_error}")
endif()

string(REGEX MATCHALL
  "(^|\n)  (http-client-[A-Za-z0-9_-]+|connector-[A-Za-z0-9_-]+|connector-unreal-[A-Za-z0-9_-]+|framework-sample-[A-Za-z0-9_-]+)"
  wildcard_label_lines
  "${print_labels_output}")
foreach(label_line IN LISTS wildcard_label_lines)
  string(REGEX REPLACE "^(\\n)?  " "" wildcard_label "${label_line}")
  string(STRIP "${wildcard_label}" wildcard_label)
  list(FIND required_labels "${wildcard_label}" required_index)
  if(required_index EQUAL -1)
    message(FATAL_ERROR
      "CTest wildcard-prefix label is not covered by required_labels: ${wildcard_label}")
  endif()
endforeach()

string(REGEX MATCHALL
  "(^|\n)  [A-Za-z0-9_-]+"
  label_lines
  "${print_labels_output}")
set(actual_labels)
foreach(label_line IN LISTS label_lines)
  string(REGEX REPLACE "^(\\n)?  " "" actual_label "${label_line}")
  string(STRIP "${actual_label}" actual_label)
  list(APPEND actual_labels "${actual_label}")
  list(FIND known_labels "${actual_label}" known_index)
  if(known_index EQUAL -1)
    message(FATAL_ERROR
      "CTest label is not covered by known label taxonomy: ${actual_label}")
  endif()
endforeach()

foreach(label IN LISTS required_labels)
  list(FIND actual_labels "${label}" actual_index)
  if(actual_index EQUAL -1)
    message(FATAL_ERROR "CTest label ${label} selects no tests")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
    -C "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}" --show-only=json-v1
  RESULT_VARIABLE test_json_result
  OUTPUT_VARIABLE test_json_output
  ERROR_VARIABLE test_json_error)
if(NOT test_json_result EQUAL 0)
  message(FATAL_ERROR "ctest test metadata print failed: ${test_json_error}")
endif()

string(JSON test_count LENGTH "${test_json_output}" tests)
if(test_count GREATER 0)
  math(EXPR test_last_index "${test_count} - 1")
  foreach(test_index RANGE ${test_last_index})
    string(JSON test_name GET "${test_json_output}" tests ${test_index} name)
    string(JSON property_count LENGTH "${test_json_output}" tests ${test_index} properties)
    set(test_tiers)
    if(property_count GREATER 0)
      math(EXPR property_last_index "${property_count} - 1")
      foreach(property_index RANGE ${property_last_index})
        string(JSON property_name GET "${test_json_output}" tests ${test_index}
          properties ${property_index} name)
        if(property_name STREQUAL "LABELS")
          string(JSON label_count LENGTH "${test_json_output}" tests ${test_index}
            properties ${property_index} value)
          if(label_count GREATER 0)
            math(EXPR label_last_index "${label_count} - 1")
            foreach(label_index RANGE ${label_last_index})
              string(JSON test_label GET "${test_json_output}" tests ${test_index}
                properties ${property_index} value ${label_index})
              list(FIND ZLINK_FRAMEWORK_CPP_TEST_TIERS "${test_label}" tier_index)
              if(NOT tier_index EQUAL -1)
                list(APPEND test_tiers "${test_label}")
              endif()
            endforeach()
          endif()
        endif()
      endforeach()
    endif()
    list(LENGTH test_tiers tier_count)
    if(NOT tier_count EQUAL 1)
      message(FATAL_ERROR
        "CTest test ${test_name} must have exactly one tier label "
        "(${ZLINK_FRAMEWORK_CPP_TEST_TIERS}); found: ${test_tiers}")
    endif()
  endforeach()
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
    -C "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}" -N
    -L "${ZLINK_FRAMEWORK_CPP_RELEASE_TEST_LABEL_REGEX}"
  RESULT_VARIABLE release_selection_result
  OUTPUT_VARIABLE release_selection_output
  ERROR_VARIABLE release_selection_error)
if(NOT release_selection_result EQUAL 0)
  message(FATAL_ERROR
    "ctest release-tier scan failed: ${release_selection_error}")
endif()
if(NOT release_selection_output MATCHES "test_cpp_stream_connector")
  message(FATAL_ERROR
    "release tier selection must include test_cpp_stream_connector")
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
    -C "${ZLINK_FRAMEWORK_CPP_CONFIGURATION}" -N -L http-client-https
  RESULT_VARIABLE http_client_https_result
  OUTPUT_VARIABLE http_client_https_output
  ERROR_VARIABLE http_client_https_error)
if(NOT http_client_https_result EQUAL 0)
  message(FATAL_ERROR
    "ctest label scan failed for http-client-https: ${http_client_https_error}")
endif()
if(NOT http_client_https_output MATCHES "test_cpp_http_client")
  message(FATAL_ERROR
    "http-client-https must select the HTTP client HTTPS regression test")
endif()
if(http_client_https_output MATCHES "test_cpp_framework_contract_headers")
  message(FATAL_ERROR
    "http-client-https must not be satisfied by public header compile smoke")
endif()
