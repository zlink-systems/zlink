function(zlink_framework_cpp_configure_shared_library target_name)
  if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
    return()
  endif()

  set_target_properties(${target_name} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    # Keep the linker input independent of its temporary build directory so
    # GNU's content-derived build ID remains reproducible after installation.
    BUILD_WITH_INSTALL_RPATH ON)
  if(WIN32)
    set_target_properties(${target_name} PROPERTIES
      MSVC_RUNTIME_LIBRARY MultiThreadedDLL
      CXX_LINKER_LAUNCHER
        "${Python3_EXECUTABLE};${ZLINK_FRAMEWORK_CPP_DIR}/cmake/filter-windows-exports.py")
  elseif(UNIX AND NOT APPLE)
    set_property(TARGET ${target_name} PROPERTY INSTALL_RPATH "$ORIGIN")
    target_link_options(${target_name} PRIVATE
      "-Wl,--exclude-libs,ALL"
      "-Wl,--version-script=${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-framework-shared-symbols.map")
  elseif(APPLE)
    set(_zlink_apple_exports
      "${CMAKE_BINARY_DIR}/zlink-framework-exported-symbols.list")
    get_property(_zlink_apple_exports_generated GLOBAL
      PROPERTY ZLINK_FRAMEWORK_CPP_APPLE_EXPORTS_GENERATED)
    if(NOT _zlink_apple_exports_generated)
      file(REMOVE "${_zlink_apple_exports}")
      file(STRINGS
        "${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-framework-shared-symbols.map"
        _zlink_apple_export_lines REGEX "^# APPLE_EXPORT: ")
      foreach(_zlink_apple_export_line IN LISTS _zlink_apple_export_lines)
        string(REGEX REPLACE "^# APPLE_EXPORT: " ""
          _zlink_apple_export "${_zlink_apple_export_line}")
        file(APPEND "${_zlink_apple_exports}" "${_zlink_apple_export}\n")
      endforeach()
      set_property(GLOBAL PROPERTY
        ZLINK_FRAMEWORK_CPP_APPLE_EXPORTS_GENERATED TRUE)
    endif()
    set_target_properties(${target_name} PROPERTIES
      INSTALL_NAME_DIR "@loader_path"
      INSTALL_RPATH "@loader_path")
    target_link_options(${target_name} PRIVATE
      "-Wl,-exported_symbols_list,${_zlink_apple_exports}")
  endif()
endfunction()
