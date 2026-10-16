# Installs the published ZLink C++ framework prebuilt for this host into
# .zlink/install, where CMakePresets.json finds it. Run it from this directory
# before configuring; run-e2e.sh, run-e2e.ps1 and the Dockerfile already do:
#
#     cmake -P bootstrap.cmake
#
# The only input is this platform's framework prebuilt attached to the
# framework-cpp/v<ZLINK_FRAMEWORK_CPP_VERSION> GitHub Release. It carries Core,
# zlink-cpp, the framework libraries, Boost headers and nlohmann_json, so no
# package manager runs. The archive must match the SHA-256 checksum published with
# it. A prefix extracted from another version is replaced.
#
# Layout (delete .zlink/ to start over):
#
#     .zlink/downloads/      the release archive and its checksum
#     .zlink/install/        extracted package prefix
cmake_minimum_required(VERSION 3.24)

set(ZLINK_FRAMEWORK_CPP_VERSION "0.30.1")
set(ZLINK_RELEASES "https://github.com/zlink-systems/zlink/releases/download")
set(ZLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.zlink")

function(zlink_fail)
  message(FATAL_ERROR "bootstrap: " ${ARGN})
endfunction()

# --- host platform -> framework prebuilt archive name -----------------------
cmake_host_system_information(RESULT _zlink_arch QUERY OS_PLATFORM)
string(TOLOWER "${_zlink_arch}" _zlink_arch)
if(_zlink_arch MATCHES "^(x86_64|amd64)$")
  set(_zlink_arch "x64")
elseif(_zlink_arch MATCHES "^(arm64|aarch64)$")
  set(_zlink_arch "arm64")
endif()
if(CMAKE_HOST_WIN32)
  set(_zlink_os "windows")
elseif(CMAKE_HOST_APPLE)
  set(_zlink_os "macos")
else()
  set(_zlink_os "linux")
endif()
set(ZLINK_PLATFORM "${_zlink_os}-${_zlink_arch}")
if(NOT ZLINK_PLATFORM MATCHES "^(windows-x64|linux-x64|linux-arm64|macos-arm64)$")
  zlink_fail("no prebuilt framework archive for ${ZLINK_PLATFORM}; "
    "the framework release ships windows-x64, linux-x64, linux-arm64 and macos-arm64")
endif()

# --- download and verify -----------------------------------------------------
set(_zlink_archive "zlink-framework-cpp-${ZLINK_FRAMEWORK_CPP_VERSION}-${ZLINK_PLATFORM}.tar.gz")
set(_zlink_url "${ZLINK_RELEASES}/framework-cpp/v${ZLINK_FRAMEWORK_CPP_VERSION}/${_zlink_archive}")
set(_zlink_downloads "${ZLINK_ROOT}/downloads")
file(MAKE_DIRECTORY "${_zlink_downloads}")

function(zlink_fetch url path)
  message(STATUS "fetching ${url}")
  file(DOWNLOAD "${url}" "${path}.part" STATUS status TLS_VERIFY ON)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    list(GET status 1 text)
    file(REMOVE "${path}.part")
    zlink_fail("could not fetch ${url} (${text})")
  endif()
  file(RENAME "${path}.part" "${path}")
endfunction()

set(_zlink_archive_path "${_zlink_downloads}/${_zlink_archive}")
set(_zlink_checksum_path "${_zlink_archive_path}.sha256")
if(NOT EXISTS "${_zlink_checksum_path}")
  zlink_fetch("${_zlink_url}.sha256" "${_zlink_checksum_path}")
endif()
if(NOT EXISTS "${_zlink_archive_path}")
  zlink_fetch("${_zlink_url}" "${_zlink_archive_path}")
endif()
file(STRINGS "${_zlink_checksum_path}" _zlink_checksum LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9a-fA-F]+" _zlink_expected "${_zlink_checksum}")
string(TOLOWER "${_zlink_expected}" _zlink_expected)
file(SHA256 "${_zlink_archive_path}" _zlink_actual)
if(NOT _zlink_actual STREQUAL _zlink_expected)
  file(REMOVE "${_zlink_archive_path}" "${_zlink_checksum_path}")
  zlink_fail("${_zlink_archive} does not match ${_zlink_archive}.sha256 "
    "(expected '${_zlink_expected}', got '${_zlink_actual}'); both files were removed")
endif()

# --- extract -----------------------------------------------------------------
set(ZLINK_INSTALL_PREFIX "${ZLINK_ROOT}/install")
set(_zlink_version_file "${ZLINK_INSTALL_PREFIX}/lib/cmake/zlink_framework/zlink_frameworkConfigVersion.cmake")
if(EXISTS "${ZLINK_INSTALL_PREFIX}")
  set(_zlink_installed "")
  if(EXISTS "${_zlink_version_file}")
    file(STRINGS "${_zlink_version_file}" _zlink_installed REGEX "^set\\(PACKAGE_VERSION ")
  endif()
  if(NOT _zlink_installed STREQUAL "set(PACKAGE_VERSION \"${ZLINK_FRAMEWORK_CPP_VERSION}\")")
    message(STATUS "replacing ${ZLINK_INSTALL_PREFIX}: not version ${ZLINK_FRAMEWORK_CPP_VERSION}")
    file(REMOVE_RECURSE "${ZLINK_INSTALL_PREFIX}")
  endif()
endif()
if(NOT EXISTS "${ZLINK_INSTALL_PREFIX}")
  message(STATUS "extracting ${_zlink_archive}")
  file(REMOVE_RECURSE "${ZLINK_INSTALL_PREFIX}.part")
  file(ARCHIVE_EXTRACT INPUT "${_zlink_archive_path}" DESTINATION "${ZLINK_INSTALL_PREFIX}.part")
  file(GLOB _zlink_roots LIST_DIRECTORIES true "${ZLINK_INSTALL_PREFIX}.part/*")
  list(LENGTH _zlink_roots _zlink_root_count)
  if(NOT _zlink_root_count EQUAL 1)
    zlink_fail("${_zlink_archive} does not have a single top-level directory")
  endif()
  file(RENAME "${_zlink_roots}" "${ZLINK_INSTALL_PREFIX}")
  file(REMOVE_RECURSE "${ZLINK_INSTALL_PREFIX}.part")
endif()
if(NOT EXISTS "${_zlink_version_file}")
  zlink_fail("the framework archive has no lib/cmake/zlink_framework/zlink_frameworkConfigVersion.cmake")
endif()
message(STATUS "framework ${ZLINK_FRAMEWORK_CPP_VERSION} installed in ${ZLINK_INSTALL_PREFIX}")
