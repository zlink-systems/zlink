# Prefer the package manager's multi-configuration target, then system LZ4.
find_package(lz4 CONFIG QUIET)
if(TARGET lz4::lz4)
  set(lz4_FOUND TRUE)
  return()
endif()
find_path(lz4_INCLUDE_DIR NAMES lz4.h)
find_library(lz4_LIBRARY NAMES lz4 liblz4)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(lz4 REQUIRED_VARS lz4_LIBRARY lz4_INCLUDE_DIR)
if(lz4_FOUND AND NOT TARGET lz4::lz4)
  add_library(lz4::lz4 UNKNOWN IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${lz4_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${lz4_INCLUDE_DIR}")
endif()
mark_as_advanced(lz4_INCLUDE_DIR lz4_LIBRARY)
