# Run against a clean checkout of the release commit. The output directory must
# be new; tracked C++ sources and shared runtime inputs are staged together.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ZLINK_SOURCE_DIR OR NOT DEFINED ZLINK_STAGE_DIR)
  message(FATAL_ERROR "Set ZLINK_SOURCE_DIR (repository) and ZLINK_STAGE_DIR (new output directory)")
endif()
if(EXISTS "${ZLINK_STAGE_DIR}")
  message(FATAL_ERROR "Staging directory already exists: ${ZLINK_STAGE_DIR}")
endif()
find_program(GIT_EXECUTABLE git REQUIRED)
find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND "${NODE_EXECUTABLE}"
  "${ZLINK_SOURCE_DIR}/framework/runtime/protocol/generate-service-wire-assets.mjs" --check
  WORKING_DIRECTORY "${ZLINK_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
file(MAKE_DIRECTORY "${ZLINK_STAGE_DIR}")
execute_process(COMMAND "${GIT_EXECUTABLE}" archive HEAD:framework/languages/cpp
  OUTPUT_FILE "${ZLINK_STAGE_DIR}/sources.tar"
  WORKING_DIRECTORY "${ZLINK_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
file(ARCHIVE_EXTRACT INPUT "${ZLINK_STAGE_DIR}/sources.tar" DESTINATION "${ZLINK_STAGE_DIR}")
file(REMOVE "${ZLINK_STAGE_DIR}/sources.tar")
# Include golden/conformance inputs so the archive can also configure tests.
execute_process(COMMAND "${GIT_EXECUTABLE}" archive HEAD:framework/runtime
  OUTPUT_FILE "${ZLINK_STAGE_DIR}/runtime.tar"
  WORKING_DIRECTORY "${ZLINK_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
file(ARCHIVE_EXTRACT INPUT "${ZLINK_STAGE_DIR}/runtime.tar" DESTINATION "${ZLINK_STAGE_DIR}/runtime")
file(REMOVE "${ZLINK_STAGE_DIR}/runtime.tar")
execute_process(COMMAND "${GIT_EXECUTABLE}" show HEAD:framework/LICENSE
  OUTPUT_FILE "${ZLINK_STAGE_DIR}/LICENSE"
  WORKING_DIRECTORY "${ZLINK_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
