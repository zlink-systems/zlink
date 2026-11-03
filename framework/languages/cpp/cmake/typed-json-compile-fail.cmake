if(MSVC)
  set(arguments /nologo /std:c++20 /EHsc /Zs "/DCASE=${CASE}"
    "/I${COMMON_INCLUDE}" "/I${JSON_INCLUDE}")
else()
  set(arguments -std=c++20 -fsyntax-only "-DCASE=${CASE}"
    "-I${COMMON_INCLUDE}" "-I${JSON_INCLUDE}")
endif()
execute_process(COMMAND "${COMPILER}" ${arguments} "${SOURCE}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
file(WRITE "${LOG}" "${output}")
if(result EQUAL 0)
  message(FATAL_ERROR "Invalid typed JSON declaration compiled: case ${CASE}")
endif()
if(NOT output MATCHES "framework-json-v1:")
  message(FATAL_ERROR "Compilation failed without the typed JSON contract diagnostic; see ${LOG}")
endif()
