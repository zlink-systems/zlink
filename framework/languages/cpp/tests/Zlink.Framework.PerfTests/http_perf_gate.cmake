if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()

set(report_path "$ENV{ZLINK_FRAMEWORK_HTTP_PERF_REPORT}")
set(require_report "$ENV{ZLINK_FRAMEWORK_HTTP_PERF_REQUIRED}")

if(report_path STREQUAL "")
  if(require_report STREQUAL "1" OR require_report STREQUAL "ON" OR require_report STREQUAL "TRUE")
    message(FATAL_ERROR
      "ZLINK_FRAMEWORK_HTTP_PERF_REPORT is required when ZLINK_FRAMEWORK_HTTP_PERF_REQUIRED is set")
  endif()
  message(STATUS
    "ZLINK_FRAMEWORK_HTTP_PERF_REPORT is not set; external Drogon/Oat++ HTTP perf gate was not evaluated")
  return()
endif()

if(NOT EXISTS "${report_path}")
  message(FATAL_ERROR "HTTP perf report does not exist: ${report_path}")
endif()

include("${report_path}")

foreach(required_variable IN ITEMS
    ZLINK_HTTP_PERF_LOAD_GENERATOR
    ZLINK_HTTP_PERF_LOAD_GENERATOR_VERSION
    ZLINK_HTTP_PERF_COMMAND_LINE
    ZLINK_HTTP_PERF_COMPILER
    ZLINK_HTTP_PERF_BUILD_TYPE
    ZLINK_HTTP_PERF_MACHINE
    ZLINK_HTTP_PERF_SCENARIOS)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "HTTP perf report is missing ${required_variable}")
  endif()
endforeach()

foreach(scenario IN LISTS ZLINK_HTTP_PERF_SCENARIOS)
  foreach(required_variable IN ITEMS
      ZLINK_HTTP_PERF_${scenario}_ZLINK_RPS
      ZLINK_HTTP_PERF_${scenario}_BASELINE_RPS
      ZLINK_HTTP_PERF_${scenario}_ZLINK_P95_US
      ZLINK_HTTP_PERF_${scenario}_BASELINE_P95_US
      ZLINK_HTTP_PERF_${scenario}_BASELINE_NAME
      ZLINK_HTTP_PERF_${scenario}_MAX_RPS_DROP_PERCENT
      ZLINK_HTTP_PERF_${scenario}_MAX_P95_SLOWDOWN_PERCENT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
      message(FATAL_ERROR "HTTP perf report scenario ${scenario} is missing ${required_variable}")
    endif()
  endforeach()

  set(zlink_rps "${ZLINK_HTTP_PERF_${scenario}_ZLINK_RPS}")
  set(baseline_rps "${ZLINK_HTTP_PERF_${scenario}_BASELINE_RPS}")
  set(zlink_p95 "${ZLINK_HTTP_PERF_${scenario}_ZLINK_P95_US}")
  set(baseline_p95 "${ZLINK_HTTP_PERF_${scenario}_BASELINE_P95_US}")
  set(max_rps_drop "${ZLINK_HTTP_PERF_${scenario}_MAX_RPS_DROP_PERCENT}")
  set(max_p95_slowdown "${ZLINK_HTTP_PERF_${scenario}_MAX_P95_SLOWDOWN_PERCENT}")

  math(EXPR min_rps_percent "100 - ${max_rps_drop}")
  math(EXPR zlink_rps_scaled "${zlink_rps} * 100")
  math(EXPR required_rps_scaled "${baseline_rps} * ${min_rps_percent}")
  if(zlink_rps_scaled LESS required_rps_scaled)
    message(FATAL_ERROR
      "HTTP perf scenario ${scenario} failed throughput gate: zlink=${zlink_rps}, baseline=${baseline_rps}, max_drop=${max_rps_drop}%")
  endif()

  math(EXPR max_p95_percent "100 + ${max_p95_slowdown}")
  math(EXPR zlink_p95_scaled "${zlink_p95} * 100")
  math(EXPR allowed_p95_scaled "${baseline_p95} * ${max_p95_percent}")
  if(zlink_p95_scaled GREATER allowed_p95_scaled)
    message(FATAL_ERROR
      "HTTP perf scenario ${scenario} failed p95 gate: zlink=${zlink_p95}us, baseline=${baseline_p95}us, max_slowdown=${max_p95_slowdown}%")
  endif()
endforeach()

message(STATUS "HTTP perf report passed: ${report_path}")
