# Only an oracle rejection (exit 1) validates the negative control. A timeout,
# exception, crash or setup failure must fail this CTest entry.
execute_process(
  COMMAND "${PROGRAM}" "--negative-${MODE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 25)
message(STATUS "negative-${MODE}: exit=${result}\n${output}${errors}")
if(NOT "${result}" STREQUAL "1")
  message(FATAL_ERROR "negative-${MODE} did not reach the expected oracle rejection")
endif()
