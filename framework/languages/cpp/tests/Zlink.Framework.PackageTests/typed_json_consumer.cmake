# Also called directly by package-prebuilt.sh against the installed shared prefix.
foreach(required IN ITEMS ZLINK_TYPED_JSON_PREFIX_PATH ZLINK_TYPED_JSON_BUILD_DIR
    ZLINK_TYPED_JSON_ENGINE)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
if(NOT DEFINED ZLINK_TYPED_JSON_CONFIGURATION)
  set(ZLINK_TYPED_JSON_CONFIGURATION Release)
endif()
string(REPLACE ";" "\\;" escaped_prefix_path "${ZLINK_TYPED_JSON_PREFIX_PATH}")
set(configure_command "${CMAKE_COMMAND}"
  -S "${CMAKE_CURRENT_LIST_DIR}/typed_json_consumer"
  -B "${ZLINK_TYPED_JSON_BUILD_DIR}"
  "-DCMAKE_PREFIX_PATH=${escaped_prefix_path}"
  "-DCMAKE_BUILD_TYPE=${ZLINK_TYPED_JSON_CONFIGURATION}"
  "-DZLINK_TYPED_JSON_ENGINE=${ZLINK_TYPED_JSON_ENGINE}")
if(DEFINED ZLINK_TYPED_JSON_CXX_COMPILER)
  list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${ZLINK_TYPED_JSON_CXX_COMPILER}")
endif()
execute_process(COMMAND ${configure_command} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "typed JSON package consumer configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ZLINK_TYPED_JSON_BUILD_DIR}"
  --config "${ZLINK_TYPED_JSON_CONFIGURATION}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "typed JSON package consumer build failed")
endif()
find_program(ctest_command ctest REQUIRED)
list(GET ZLINK_TYPED_JSON_PREFIX_PATH 0 installed_prefix)
if(WIN32)
  set(runtime_environment "PATH=${installed_prefix}/bin;$ENV{PATH}")
elseif(APPLE)
  set(runtime_environment "DYLD_LIBRARY_PATH=${installed_prefix}/lib:$ENV{DYLD_LIBRARY_PATH}")
else()
  set(runtime_environment "LD_LIBRARY_PATH=${installed_prefix}/lib:$ENV{LD_LIBRARY_PATH}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${runtime_environment}"
  "${ctest_command}" --test-dir "${ZLINK_TYPED_JSON_BUILD_DIR}"
  -C "${ZLINK_TYPED_JSON_CONFIGURATION}" --output-on-failure
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "typed JSON ${ZLINK_TYPED_JSON_ENGINE} package consumer exit failed")
endif()
