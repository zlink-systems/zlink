# A distributable library exposes exactly the zlink:: surface named in
# zlink-framework-shared-symbols.map, whether it is shared or static. The
# shared library gets it from the linker; a static archive has no linker step
# of its own, so the same list rewrites the finished archive. Without that the
# archive carries every Boost template instantiation as an ordinary external
# definition, and a consumer that compiles its own Boost from different headers
# collapses the two onto one definition (issue #2133, the same failure as Core
# issue #418). MSVC is left out as it is for libzlink.lib: link.exe has no
# relocatable link and lib.exe no visibility control, so a static .lib cannot be
# rewritten.
if(NOT ZLINK_FRAMEWORK_CPP_SHARED
    AND NOT MSVC
    AND NOT MINGW
    AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
  set(ZLINK_FRAMEWORK_CPP_HIDES_STATIC_ARCHIVES ON)
  find_program(ZLINK_BASH_EXECUTABLE NAMES bash)
  # The tools are Core's (#418). A source archive carries copies of the three
  # scripts next to this project; a repository checkout reads them in place.
  if(EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/tools/hide_static_archive_internals.sh")
    set(ZLINK_STATIC_ARCHIVE_TOOLS_DIR "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/tools")
  else()
    set(ZLINK_STATIC_ARCHIVE_TOOLS_DIR "${ZLINK_FRAMEWORK_CPP_DIR}/tools")
  endif()
  set(zlink_static_hide_tools ZLINK_BASH_EXECUTABLE CMAKE_AR CMAKE_RANLIB
                              CMAKE_LINKER CMAKE_NM)
  if(NOT APPLE)
    list(APPEND zlink_static_hide_tools CMAKE_OBJCOPY)
  endif()
  foreach(tool IN LISTS zlink_static_hide_tools)
    if(NOT ${tool})
      message(FATAL_ERROR
        "${tool} is not available, so a static archive cannot be restricted to "
        "the zlink:: surface. An archive that also exports the Boost it was "
        "built with crashes consumers that link a different Boost (issue #2133).")
    endif()
  endforeach()
else()
  set(ZLINK_FRAMEWORK_CPP_HIDES_STATIC_ARCHIVES OFF)
endif()

# Called after the install(TARGETS) that installs `target_name`, so the rewrite
# runs on the installed copy. The build-tree archive stays whole: the in-tree
# tests include the private headers and drive the library's Boost.Asio objects
# with their own, which only works while the test and the library share one
# copy of Boost's statics. A consumer of the installed package never does.
function(zlink_framework_cpp_restrict_installed_static_archive target_name component)
  if(NOT ZLINK_FRAMEWORK_CPP_HIDES_STATIC_ARCHIVES)
    return()
  endif()
  install(CODE "
    execute_process(
      COMMAND \"${CMAKE_COMMAND}\" -E env \"AR=${CMAKE_AR}\" \"RANLIB=${CMAKE_RANLIB}\"
        \"LD=${CMAKE_LINKER}\" \"NM=${CMAKE_NM}\" \"OBJCOPY=${CMAKE_OBJCOPY}\"
        \"${ZLINK_BASH_EXECUTABLE}\"
        \"${ZLINK_STATIC_ARCHIVE_TOOLS_DIR}/hide_static_archive_internals.sh\"
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${target_name}>\"
        \"${ZLINK_FRAMEWORK_CPP_DIR}/cmake/zlink-framework-shared-symbols.map\"
      RESULT_VARIABLE zlink_hide_result)
    if(NOT zlink_hide_result EQUAL 0)
      message(FATAL_ERROR \"restricting $<TARGET_FILE_NAME:${target_name}> to the zlink:: surface failed\")
    endif()
  " COMPONENT ${component})
endfunction()

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
