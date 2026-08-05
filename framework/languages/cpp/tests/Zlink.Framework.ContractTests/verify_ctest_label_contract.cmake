if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()

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
  connector-perf-smoke
  connector-perf-scale
  connector-unreal-contract
  connector-unreal-compile
  connector-unreal-smoke
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
  framework-sample-mission
  framework-sample-play
  framework-sample-registry
  framework-sample-session
  framework-sample-smoke
  framework-sample-shoppingmall
  framework-sample-support
  framework-sample-supportchat
  framework-sample-tictactoe
  framework-sample-tracking
  framework-sample-workflow)

file(READ "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/CMakeCache.txt" build_cache)
if(build_cache MATCHES "ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT:BOOL=ON")
  list(APPEND required_labels framework-http-perf)
endif()
if(build_cache MATCHES "ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES:BOOL=ON")
  list(APPEND required_labels ${sample_labels})
endif()

set(known_labels
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
  async
  backpressure
  channel
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
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}" --print-labels
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
foreach(label_line IN LISTS label_lines)
  string(REGEX REPLACE "^(\\n)?  " "" actual_label "${label_line}")
  string(STRIP "${actual_label}" actual_label)
  list(FIND known_labels "${actual_label}" known_index)
  if(known_index EQUAL -1)
    message(FATAL_ERROR
      "CTest label is not covered by known label taxonomy: ${actual_label}")
  endif()
endforeach()

foreach(label IN LISTS required_labels)
  execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}" -N -L "${label}"
    RESULT_VARIABLE ctest_result
    OUTPUT_VARIABLE ctest_output
    ERROR_VARIABLE ctest_error)
  if(NOT ctest_result EQUAL 0)
    message(FATAL_ERROR "ctest label scan failed for ${label}: ${ctest_error}")
  endif()
  if(NOT ctest_output MATCHES "Total Tests: *([0-9]+)")
    message(FATAL_ERROR "ctest label scan did not report a test count for ${label}")
  endif()
  set(test_count "${CMAKE_MATCH_1}")
  if(test_count LESS 1)
    message(FATAL_ERROR "CTest label ${label} selects no tests")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}" -N -L http-client-https
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
