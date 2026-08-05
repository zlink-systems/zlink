if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD)
  set(ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD 80)
endif()
if(NOT DEFINED ZLINK_GCOV_EXECUTABLE)
  set(ZLINK_GCOV_EXECUTABLE gcov)
endif()

set(gcov_work_dir "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/coverage-gcov")
file(REMOVE_RECURSE "${gcov_work_dir}")
file(MAKE_DIRECTORY "${gcov_work_dir}")

set(gcda_files)
set(coverage_target_globs
  zlink_framework
  zlink_http_client
  zlink_stream_connector
  zlink_unreal_stream_connector
  "test_cpp_framework*"
  test_cpp_http_client
  test_cpp_stream_connector)

foreach(target_glob IN LISTS coverage_target_globs)
  file(GLOB_RECURSE target_gcda
    "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/CMakeFiles/${target_glob}.dir/*.gcda")
  list(APPEND gcda_files ${target_gcda})
endforeach()

if(NOT gcda_files)
  message(FATAL_ERROR
    "No coverage data found. Configure with -DZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE=ON and run tests first.")
endif()

set(gcov_report_dirs)
foreach(gcda IN LISTS gcda_files)
  get_filename_component(gcda_dir "${gcda}" DIRECTORY)
  string(MD5 gcda_key "${gcda}")
  set(gcov_object_dir "${gcov_work_dir}/${gcda_key}")
  file(MAKE_DIRECTORY "${gcov_object_dir}")
  list(APPEND gcov_report_dirs "${gcov_object_dir}")
  execute_process(
    COMMAND "${ZLINK_GCOV_EXECUTABLE}" -o "${gcda_dir}" "${gcda}"
    WORKING_DIRECTORY "${gcov_object_dir}"
    RESULT_VARIABLE gcov_result
    OUTPUT_QUIET
    ERROR_VARIABLE gcov_error)
  if(NOT gcov_result EQUAL 0)
    message(FATAL_ERROR "gcov failed for ${gcda}: ${gcov_error}")
  endif()
endforeach()

set(gcov_reports)
foreach(gcov_report_dir IN LISTS gcov_report_dirs)
  file(GLOB object_reports "${gcov_report_dir}/*.gcov")
  list(APPEND gcov_reports ${object_reports})
endforeach()
set(covered_lines 0)
set(total_lines 0)
set(source_files_seen 0)

foreach(report IN LISTS gcov_reports)
  file(READ "${report}" report_text)
  string(REPLACE "\n" ";" report_lines "${report_text}")
  set(source_path "")
  foreach(line IN LISTS report_lines)
    if(line MATCHES "^ *-: *0:Source:(.*)$")
      set(source_path "${CMAKE_MATCH_1}")
      file(REAL_PATH "${source_path}" source_path
        BASE_DIRECTORY "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}")
      break()
    endif()
  endforeach()

  if(NOT source_path MATCHES
      "^${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/(framework/src|http-client/src|connector/core/src|connector/engines/unreal/Source|extensions/framework-locations-redis/include)/.*\\.(cpp|hpp)$")
    continue()
  endif()

  string(MD5 source_key "${source_path}")
  if(NOT DEFINED seen_source_${source_key})
    set(seen_source_${source_key} TRUE)
    math(EXPR source_files_seen "${source_files_seen} + 1")
  endif()

  foreach(line IN LISTS report_lines)
    if(NOT line MATCHES "^ *([^:]+): *([0-9]+):")
      continue()
    endif()
    set(count "${CMAKE_MATCH_1}")
    set(line_no "${CMAKE_MATCH_2}")
    string(STRIP "${count}" count)
    if(count STREQUAL "-")
      continue()
    endif()

    string(MD5 line_key "${source_path}:${line_no}")
    if(NOT DEFINED seen_line_${line_key})
      set(seen_line_${line_key} TRUE)
      math(EXPR total_lines "${total_lines} + 1")
    endif()

    if(NOT count MATCHES "^(0|#+|=+)$" AND NOT DEFINED covered_line_${line_key})
      set(covered_line_${line_key} TRUE)
      math(EXPR covered_lines "${covered_lines} + 1")
    endif()
  endforeach()
endforeach()

if(total_lines EQUAL 0)
  message(FATAL_ERROR "No runtime source lines were found in coverage reports.")
endif()

math(EXPR coverage_times_100
  "(${covered_lines} * 10000 + (${total_lines} / 2)) / ${total_lines}")
math(EXPR coverage_integer "${coverage_times_100} / 100")
math(EXPR coverage_fraction "${coverage_times_100} % 100")

message(STATUS
  "C++ framework runtime line coverage: ${coverage_integer}.${coverage_fraction}% (${covered_lines}/${total_lines}, ${source_files_seen} files)")

math(EXPR threshold_times_100
  "${ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD} * 100")
if(coverage_times_100 LESS threshold_times_100)
  message(FATAL_ERROR
    "C++ framework runtime line coverage ${coverage_integer}.${coverage_fraction}% is below ${ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD}%")
endif()
