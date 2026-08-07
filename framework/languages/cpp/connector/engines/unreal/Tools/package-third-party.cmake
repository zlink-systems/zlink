# SPDX-License-Identifier: FSL-1.1-ALv2
#
# Stage the CMake-built native connector and its link/runtime dependencies in
# the Unreal plugin's ThirdParty directory. The generated manifest is consumed
# by ZLinkStreamConnector.Build.cs; Unreal does not need to understand the
# CMake target graph.

if(NOT DEFINED ZLINK_UNREAL_BUILD_DIR OR NOT DEFINED ZLINK_UNREAL_OUTPUT_DIR)
  message(FATAL_ERROR
    "ZLINK_UNREAL_BUILD_DIR and ZLINK_UNREAL_OUTPUT_DIR are required")
endif()

get_filename_component(ZLINK_UNREAL_BUILD_DIR
  "${ZLINK_UNREAL_BUILD_DIR}" ABSOLUTE)
get_filename_component(ZLINK_UNREAL_OUTPUT_DIR
  "${ZLINK_UNREAL_OUTPUT_DIR}" ABSOLUTE)
if(NOT DEFINED ZLINK_UNREAL_CONFIGURATION)
  set(ZLINK_UNREAL_CONFIGURATION Release)
endif()

if(NOT EXISTS "${ZLINK_UNREAL_BUILD_DIR}/CMakeCache.txt")
  message(FATAL_ERROR
    "The Unreal package build directory has no CMakeCache.txt: "
    "${ZLINK_UNREAL_BUILD_DIR}")
endif()

get_filename_component(ZLINK_UNREAL_PLUGIN_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(ZLINK_UNREAL_OUTPUT_DIR STREQUAL "/"
    OR ZLINK_UNREAL_OUTPUT_DIR STREQUAL "${ZLINK_UNREAL_PLUGIN_DIR}")
  message(FATAL_ERROR
    "Refusing to replace the Unreal plugin or filesystem root: "
    "${ZLINK_UNREAL_OUTPUT_DIR}")
endif()

file(STRINGS "${ZLINK_UNREAL_BUILD_DIR}/CMakeCache.txt" ZLINK_UNREAL_CACHE_LINES)
set(ZLINK_UNREAL_UNREAL_ENABLED FALSE)
foreach(cache_line IN LISTS ZLINK_UNREAL_CACHE_LINES)
  if(cache_line MATCHES "^ZLINK_STREAM_CONNECTOR_BUILD_UNREAL:[^=]+=ON$")
    set(ZLINK_UNREAL_UNREAL_ENABLED TRUE)
  endif()
endforeach()
if(NOT ZLINK_UNREAL_UNREAL_ENABLED)
  message(FATAL_ERROR
    "The build directory was configured without "
    "ZLINK_STREAM_CONNECTOR_BUILD_UNREAL=ON")
endif()

file(REMOVE "${ZLINK_UNREAL_OUTPUT_DIR}/zlink-unreal-package.manifest")
foreach(package_directory IN ITEMS include lib bin)
  file(REMOVE_RECURSE "${ZLINK_UNREAL_OUTPUT_DIR}/${package_directory}")
endforeach()
file(MAKE_DIRECTORY "${ZLINK_UNREAL_OUTPUT_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${ZLINK_UNREAL_BUILD_DIR}"
    --prefix "${ZLINK_UNREAL_OUTPUT_DIR}"
    --component StreamConnector
    --config "${ZLINK_UNREAL_CONFIGURATION}"
  RESULT_VARIABLE ZLINK_UNREAL_INSTALL_RESULT
  OUTPUT_VARIABLE ZLINK_UNREAL_INSTALL_OUTPUT
  ERROR_VARIABLE ZLINK_UNREAL_INSTALL_ERROR)
if(NOT ZLINK_UNREAL_INSTALL_RESULT EQUAL 0)
  message(STATUS "native install output:\n${ZLINK_UNREAL_INSTALL_OUTPUT}")
  message(FATAL_ERROR
    "CMake install for the Unreal package failed:\n${ZLINK_UNREAL_INSTALL_ERROR}")
endif()

function(zlink_unreal_require_artifact pattern output_variable)
  file(GLOB candidates LIST_DIRECTORIES FALSE
    "${ZLINK_UNREAL_OUTPUT_DIR}/lib/${pattern}"
    "${ZLINK_UNREAL_OUTPUT_DIR}/bin/${pattern}")
  list(SORT candidates)
  if(NOT candidates)
    message(FATAL_ERROR
      "The Unreal package is missing the native artifact matching ${pattern}")
  endif()
  list(GET candidates 0 candidate)
  file(RELATIVE_PATH relative_path "${ZLINK_UNREAL_OUTPUT_DIR}" "${candidate}")
  set(${output_variable} "${relative_path}" PARENT_SCOPE)
endfunction()

function(zlink_unreal_append_unique list_variable value)
  set(values "${${list_variable}}")
  list(FIND values "${value}" existing_index)
  if(existing_index EQUAL -1)
    list(APPEND values "${value}")
  endif()
  set(${list_variable} "${values}" PARENT_SCOPE)
endfunction()

function(zlink_unreal_cache_value cache_key output_variable)
  set(found_value "")
  foreach(cache_line IN LISTS ZLINK_UNREAL_CACHE_LINES)
    if(cache_line MATCHES "^${cache_key}:[^=]*=(.*)$")
      set(found_value "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set(${output_variable} "${found_value}" PARENT_SCOPE)
endfunction()

set(ZLINK_UNREAL_COMPILER_ID "")
set(ZLINK_UNREAL_COMPILER_VERSION "")
set(ZLINK_UNREAL_PLATFORM "")
set(ZLINK_UNREAL_ARCHITECTURE "")
file(GLOB ZLINK_UNREAL_COMPILER_FILES LIST_DIRECTORIES FALSE
  "${ZLINK_UNREAL_BUILD_DIR}/CMakeFiles/*/CMakeCXXCompiler.cmake")
list(SORT ZLINK_UNREAL_COMPILER_FILES)
if(ZLINK_UNREAL_COMPILER_FILES)
  list(GET ZLINK_UNREAL_COMPILER_FILES -1 ZLINK_UNREAL_COMPILER_FILE)
  file(STRINGS "${ZLINK_UNREAL_COMPILER_FILE}" compiler_lines)
  foreach(compiler_line IN LISTS compiler_lines)
    if(compiler_line MATCHES "set\\(CMAKE_CXX_COMPILER_ID \"([^\"]+)\"\\)")
      set(ZLINK_UNREAL_COMPILER_ID "${CMAKE_MATCH_1}")
    elseif(compiler_line MATCHES "set\\(CMAKE_CXX_COMPILER_VERSION \"([^\"]+)\"\\)")
      set(ZLINK_UNREAL_COMPILER_VERSION "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()

set(ZLINK_UNREAL_SYSTEM_FILES)
file(GLOB ZLINK_UNREAL_SYSTEM_FILES LIST_DIRECTORIES FALSE
  "${ZLINK_UNREAL_BUILD_DIR}/CMakeFiles/*/CMakeSystem.cmake")
list(SORT ZLINK_UNREAL_SYSTEM_FILES)
if(ZLINK_UNREAL_SYSTEM_FILES)
  list(GET ZLINK_UNREAL_SYSTEM_FILES -1 ZLINK_UNREAL_SYSTEM_FILE)
  file(STRINGS "${ZLINK_UNREAL_SYSTEM_FILE}" system_lines)
  foreach(system_line IN LISTS system_lines)
    if(system_line MATCHES "set\\(CMAKE_SYSTEM_NAME \"([^\"]+)\"\\)")
      set(ZLINK_UNREAL_PLATFORM "${CMAKE_MATCH_1}")
    elseif(system_line MATCHES "set\\(CMAKE_SYSTEM_PROCESSOR \"([^\"]+)\"\\)")
      set(ZLINK_UNREAL_ARCHITECTURE "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()

string(TOLOWER "${ZLINK_UNREAL_PLATFORM}" ZLINK_UNREAL_PLATFORM)
if(ZLINK_UNREAL_PLATFORM MATCHES "windows")
  set(ZLINK_UNREAL_PLATFORM windows)
elseif(ZLINK_UNREAL_PLATFORM MATCHES "darwin|macos")
  set(ZLINK_UNREAL_PLATFORM darwin)
elseif(ZLINK_UNREAL_PLATFORM MATCHES "linux")
  set(ZLINK_UNREAL_PLATFORM linux)
endif()
string(TOLOWER "${ZLINK_UNREAL_ARCHITECTURE}" ZLINK_UNREAL_ARCHITECTURE)
if(ZLINK_UNREAL_ARCHITECTURE MATCHES "^(x86_64|amd64|x64)$")
  set(ZLINK_UNREAL_ARCHITECTURE x86_64)
elseif(ZLINK_UNREAL_ARCHITECTURE MATCHES "^(aarch64|arm64)$")
  set(ZLINK_UNREAL_ARCHITECTURE arm64)
endif()
string(TOLOWER "${ZLINK_UNREAL_CONFIGURATION}" ZLINK_UNREAL_CONFIGURATION_METADATA)
if(ZLINK_UNREAL_CONFIGURATION_METADATA MATCHES "debug")
  set(ZLINK_UNREAL_CONFIGURATION_METADATA debug)
else()
  set(ZLINK_UNREAL_CONFIGURATION_METADATA release)
endif()
if(NOT ZLINK_UNREAL_PLATFORM OR NOT ZLINK_UNREAL_ARCHITECTURE
    OR NOT ZLINK_UNREAL_COMPILER_ID OR NOT ZLINK_UNREAL_COMPILER_VERSION)
  message(FATAL_ERROR
    "Could not determine Unreal package target metadata from the CMake build")
endif()

set(ZLINK_UNREAL_MANIFEST_LIBRARIES)
set(ZLINK_UNREAL_MANIFEST_RUNTIMES)
set(ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES)

foreach(native_target IN ITEMS
    "*zlink_unreal_stream_connector*"
    "*zlink_stream_connector*"
    "*zlink_cpp*")
  zlink_unreal_require_artifact("${native_target}" native_artifact)
  zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_LIBRARIES "${native_artifact}")
endforeach()

# The Core shared library is both a link input for the static bindings and a
# staged runtime dependency. Windows uses its import library from lib/ and DLL
# from bin/; Unix uses the shared object directly.
file(GLOB core_link_artifacts LIST_DIRECTORIES FALSE
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink.so*"
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink.dylib*"
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink*.lib"
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink*.dll.a")
file(GLOB core_runtime_artifacts LIST_DIRECTORIES FALSE
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink.so*"
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink.dylib*"
  "${ZLINK_UNREAL_OUTPUT_DIR}/bin/*zlink*.dll")
foreach(native_artifact IN LISTS core_link_artifacts)
  if(NOT IS_SYMLINK "${native_artifact}")
    file(RELATIVE_PATH relative_path "${ZLINK_UNREAL_OUTPUT_DIR}" "${native_artifact}")
    zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_LIBRARIES "${relative_path}")
  endif()
endforeach()
foreach(native_artifact IN LISTS core_runtime_artifacts)
  if(NOT IS_SYMLINK "${native_artifact}")
    file(RELATIVE_PATH relative_path "${ZLINK_UNREAL_OUTPUT_DIR}" "${native_artifact}")
    zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_RUNTIMES "${relative_path}")
  endif()
endforeach()
if(NOT ZLINK_UNREAL_MANIFEST_RUNTIMES)
  message(FATAL_ERROR "The Unreal package has no staged Core runtime")
endif()

# Static OpenSSL and LZ4 builds are common in vcpkg-based configurations. Copy
# the exact libraries selected by the CMake cache so the Unreal module does not
# depend on an absolute path into the producer's build tree.
foreach(cache_key IN ITEMS OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY ZLINK_LZ4_LIBRARY)
  zlink_unreal_cache_value("${cache_key}" dependency_path)
  if(dependency_path AND EXISTS "${dependency_path}")
    file(COPY "${dependency_path}" DESTINATION "${ZLINK_UNREAL_OUTPUT_DIR}/lib")
    get_filename_component(dependency_name "${dependency_path}" NAME)
    set(staged_dependency "lib/${dependency_name}")
    zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_LIBRARIES "${staged_dependency}")
    if(dependency_name MATCHES "\\.(dll|so|dylib)(\\.|$)")
      zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_RUNTIMES "${staged_dependency}")
    endif()
  endif()
endforeach()

# Copy shared OpenSSL runtime files when the selected import/library files are
# dynamic. Static dependencies remain link-only entries in the manifest.
zlink_unreal_cache_value("OPENSSL_ROOT_DIR" openssl_root)
if(openssl_root AND EXISTS "${openssl_root}")
  file(GLOB openssl_runtime_files LIST_DIRECTORIES FALSE
    "${openssl_root}/bin/*ssl*.dll"
    "${openssl_root}/bin/*crypto*.dll"
    "${openssl_root}/lib/*ssl*.so*"
    "${openssl_root}/lib/*crypto*.so*")
  foreach(runtime_file IN LISTS openssl_runtime_files)
    file(COPY "${runtime_file}" DESTINATION "${ZLINK_UNREAL_OUTPUT_DIR}/bin")
    get_filename_component(runtime_name "${runtime_file}" NAME)
    if(runtime_name MATCHES "\\.(dll|so|dylib)(\\.|$)")
      zlink_unreal_append_unique(ZLINK_UNREAL_MANIFEST_RUNTIMES "bin/${runtime_name}")
    endif()
  endforeach()
endif()

function(zlink_unreal_append_core_system_libraries output_variable)
  set(system_libraries "${${output_variable}}")
  zlink_unreal_cache_value(
    "ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX" core_prefix)
  if(core_prefix)
    file(GLOB core_export_files LIST_DIRECTORIES FALSE
      "${core_prefix}/lib/cmake/zlink/zlinkTargets.cmake")
    foreach(core_export_file IN LISTS core_export_files)
      file(STRINGS "${core_export_file}" core_export_lines)
      foreach(core_export_line IN LISTS core_export_lines)
        if(core_export_line MATCHES
            "INTERFACE_LINK_LIBRARIES \"([^\"]+)\"")
          set(core_dependencies "${CMAKE_MATCH_1}")
          foreach(core_dependency IN LISTS core_dependencies)
            set(system_name "")
            if(core_dependency MATCHES "^-l(.+)$")
              set(system_name "${CMAKE_MATCH_1}")
            elseif(core_dependency MATCHES "^[A-Za-z0-9_.+-]+$")
              set(system_name "${core_dependency}")
            endif()
            if(system_name)
              zlink_unreal_append_unique(system_libraries "${system_name}")
            endif()
          endforeach()
        endif()
      endforeach()
    endforeach()
  endif()
  set(${output_variable} "${system_libraries}" PARENT_SCOPE)
endfunction()

# Select dependencies for the staged target, not for the host running this
# script. This matters for a Windows or macOS cross-build packaged on Linux.
if(ZLINK_UNREAL_PLATFORM STREQUAL windows)
  list(APPEND ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES
    ws2_32.lib mswsock.lib crypt32.lib iphlpapi.lib rpcrt4.lib)
elseif(ZLINK_UNREAL_PLATFORM STREQUAL darwin)
  list(APPEND ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES c++)
elseif(ZLINK_UNREAL_PLATFORM STREQUAL linux)
  # These are the common system libraries used by the connector link graph.
  # Optional Core libraries such as libbsd are appended only when the Core
  # package export says that the configured target uses them.
  list(APPEND ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES
    pthread dl m z)
else()
  message(FATAL_ERROR
    "Unsupported Unreal package target platform: ${ZLINK_UNREAL_PLATFORM}")
endif()
zlink_unreal_append_core_system_libraries(ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES)
list(REMOVE_DUPLICATES ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES)

set(manifest_path "${ZLINK_UNREAL_OUTPUT_DIR}/zlink-unreal-package.manifest")
file(WRITE "${manifest_path}"
  "# Generated by Tools/package-third-party.cmake; do not edit.\n"
  "schema=1\n"
  "platform=${ZLINK_UNREAL_PLATFORM}\n"
  "architecture=${ZLINK_UNREAL_ARCHITECTURE}\n"
  "configuration=${ZLINK_UNREAL_CONFIGURATION_METADATA}\n"
  "compiler_id=${ZLINK_UNREAL_COMPILER_ID}\n"
  "compiler_version=${ZLINK_UNREAL_COMPILER_VERSION}\n"
  "cxx_standard=20\n"
  "include=include\n")
foreach(manifest_library IN LISTS ZLINK_UNREAL_MANIFEST_LIBRARIES)
  file(APPEND "${manifest_path}" "library=${manifest_library}\n")
endforeach()
foreach(manifest_runtime IN LISTS ZLINK_UNREAL_MANIFEST_RUNTIMES)
  file(APPEND "${manifest_path}" "runtime=${manifest_runtime}\n")
endforeach()
foreach(system_library IN LISTS ZLINK_UNREAL_MANIFEST_SYSTEM_LIBRARIES)
  file(APPEND "${manifest_path}" "system_library=${system_library}\n")
endforeach()

if(NOT EXISTS "${ZLINK_UNREAL_OUTPUT_DIR}/include")
  message(FATAL_ERROR "The Unreal package has no include directory")
endif()
file(GLOB unrelated_framework_artifacts LIST_DIRECTORIES FALSE
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink_framework*"
  "${ZLINK_UNREAL_OUTPUT_DIR}/lib/*zlink_http_client*")
if(unrelated_framework_artifacts)
  message(FATAL_ERROR
    "The connector-only Unreal package contains unrelated framework artifacts")
endif()
foreach(staged_file IN LISTS ZLINK_UNREAL_MANIFEST_LIBRARIES ZLINK_UNREAL_MANIFEST_RUNTIMES)
  if(NOT EXISTS "${ZLINK_UNREAL_OUTPUT_DIR}/${staged_file}")
    message(FATAL_ERROR "The Unreal package manifest references a missing file: ${staged_file}")
  endif()
endforeach()

message(STATUS "Unreal native package: ${ZLINK_UNREAL_OUTPUT_DIR}")
message(STATUS "Unreal package manifest: ${manifest_path}")
