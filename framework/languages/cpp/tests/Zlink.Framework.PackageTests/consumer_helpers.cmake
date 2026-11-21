# CMake expands unescaped semicolons in COMMAND lists into separate arguments.
function(zlink_framework_cpp_escape_list_argument output_variable list_value)
  string(REPLACE ";" "\;" escaped_list_value "${list_value}")
  set(${output_variable} "${escaped_list_value}" PARENT_SCOPE)
endfunction()
