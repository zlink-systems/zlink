execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ls-files -z --cached --others --exclude-standard -- .
  RESULT_VARIABLE result
  OUTPUT_FILE "${MANIFEST}")
if(NOT result EQUAL 0)
  file(REMOVE "${MANIFEST}")
  message(FATAL_ERROR "Cannot enumerate layout source files: git ls-files exited ${result}")
endif()
