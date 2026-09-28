# Downloads the published ZLink C++ framework prebuilt for this host, then
# configures this project against it. Run once from this directory:
#
#     cmake -P bootstrap.cmake
#
# The sole input is this platform's framework prebuilt from
# framework-cpp/v<FRAMEWORK>. It includes Core, zlink-cpp, the framework
# libraries, and nlohmann_json; this script neither reads nor builds zlink.
#
# Output layout (.zlink/ beside this file; delete it to start over):
#
#     .zlink/downloads/      the framework release asset
#     .zlink/install/        extracted package prefix
#     build/                 this project, configured
#
# Options (pass as -D<name>=<value> before -P):
#     ZLINK_ROOT   where .zlink/ goes (default: beside this file)
cmake_minimum_required(VERSION 3.24)

set(ZLINK_FRAMEWORK_CPP_VERSION "0.26.0")
set(ZLINK_RELEASES "https://github.com/zlink-systems/zlink/releases/download")

set(ZLINK_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT ZLINK_ROOT)
  set(ZLINK_ROOT "${ZLINK_PROJECT_DIR}/.zlink")
endif()
file(MAKE_DIRECTORY "${ZLINK_ROOT}/downloads")

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

# --- generator ---------------------------------------------------------------
set(ZLINK_CONFIG Release)
if(CMAKE_HOST_WIN32)
  set(_zlink_vswhere "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(NOT EXISTS "${_zlink_vswhere}")
    set(_zlink_vswhere "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
  endif()
  if(NOT EXISTS "${_zlink_vswhere}")
    zlink_fail("could not find vswhere.exe; install Visual Studio 2022 or 2026 with the 'Desktop development with C++' workload")
  endif()
  foreach(_zlink_prerelease "" "-prerelease")
    execute_process(COMMAND "${_zlink_vswhere}" -latest ${_zlink_prerelease} -products * -version "[17.0,19.0)"
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
      -property installationVersion OUTPUT_VARIABLE _zlink_vs_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_zlink_vs_version)
      break()
    endif()
  endforeach()
  if(NOT _zlink_vs_version MATCHES "^(17|18)\\.")
    zlink_fail("could not find Visual Studio 2022 or 2026 with the C++ tools (vswhere found none); install the 'Desktop development with C++' workload")
  endif()
  if(CMAKE_MATCH_1 STREQUAL "18")
    set(ZLINK_GENERATOR -G "Visual Studio 18 2026" -A x64)
  else()
    set(ZLINK_GENERATOR -G "Visual Studio 17 2022" -A x64)
  endif()
  set(ZLINK_BUILD_CONFIG --config ${ZLINK_CONFIG})
  set(ZLINK_PROJECT_CONFIG_ARGS "")
else()
  find_program(_zlink_ninja ninja)
  if(_zlink_ninja)
    set(ZLINK_GENERATOR -G Ninja)
  else()
    set(ZLINK_GENERATOR -G "Unix Makefiles")
  endif()
  set(ZLINK_BUILD_CONFIG "")
  set(ZLINK_PROJECT_CONFIG_ARGS "-DCMAKE_BUILD_TYPE=${ZLINK_CONFIG}")
endif()

# --- download and extract ----------------------------------------------------
function(zlink_download name url)
  set(path "${ZLINK_ROOT}/downloads/${name}")
  if(EXISTS "${path}")
    message(STATUS "have ${name}")
    return()
  endif()
  message(STATUS "downloading ${url}")
  file(DOWNLOAD "${url}" "${path}.part" STATUS status TLS_VERIFY ON)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    list(GET status 1 text)
    file(REMOVE "${path}.part")
    zlink_fail("framework prebuilt is unavailable for this host platform: ${url} (${text})")
  endif()
  file(RENAME "${path}.part" "${path}")
endfunction()

function(zlink_extract archive destination)
  if(EXISTS "${destination}")
    return()
  endif()
  message(STATUS "extracting ${archive}")
  file(ARCHIVE_EXTRACT INPUT "${ZLINK_ROOT}/downloads/${archive}"
    DESTINATION "${destination}.part")
  file(GLOB roots LIST_DIRECTORIES true "${destination}.part/*")
  list(LENGTH roots count)
  if(NOT count EQUAL 1)
    zlink_fail("${archive} does not have a single top-level directory")
  endif()
  file(RENAME "${roots}" "${destination}")
  file(REMOVE_RECURSE "${destination}.part")
endfunction()

function(zlink_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code)
  if(NOT code EQUAL 0)
    list(JOIN ARGN " " text)
    zlink_fail("command failed (${code}): ${text}")
  endif()
endfunction()

# --- framework prebuilt ------------------------------------------------------
set(_zlink_framework_archive
  "zlink-framework-cpp-${ZLINK_FRAMEWORK_CPP_VERSION}-${ZLINK_PLATFORM}.tar.gz")
zlink_download("${_zlink_framework_archive}"
  "${ZLINK_RELEASES}/framework-cpp/v${ZLINK_FRAMEWORK_CPP_VERSION}/${_zlink_framework_archive}")
set(ZLINK_INSTALL_PREFIX "${ZLINK_ROOT}/install")
zlink_extract("${_zlink_framework_archive}" "${ZLINK_INSTALL_PREFIX}")
if(NOT EXISTS "${ZLINK_INSTALL_PREFIX}/lib/cmake/zlink_framework/zlink_frameworkConfig.cmake")
  zlink_fail("the framework archive has no lib/cmake/zlink_framework/zlink_frameworkConfig.cmake")
endif()

# --- this project ------------------------------------------------------------
message(STATUS "== configuring ${ZLINK_PROJECT_DIR} against ${ZLINK_INSTALL_PREFIX}")
set(_zlink_project_cache
  ${ZLINK_PROJECT_CONFIG_ARGS}
  -DCMAKE_CXX_STANDARD=20
  -DCMAKE_CXX_STANDARD_REQUIRED=ON
  "-DCMAKE_PREFIX_PATH=${ZLINK_INSTALL_PREFIX}")
zlink_run("${CMAKE_COMMAND}" -S "${ZLINK_PROJECT_DIR}" -B "${ZLINK_PROJECT_DIR}/build"
  ${ZLINK_GENERATOR} ${_zlink_project_cache})

# --- IDE preset --------------------------------------------------------------
set(_zlink_preset_cache "")
foreach(_zlink_argument IN LISTS _zlink_project_cache)
  if(_zlink_argument MATCHES "^-D([^=]+)=(.*)$")
    string(APPEND _zlink_preset_cache
      "        \"${CMAKE_MATCH_1}\": \"${CMAKE_MATCH_2}\",\n")
  endif()
endforeach()
string(REGEX REPLACE ",\n$" "\n" _zlink_preset_cache "${_zlink_preset_cache}")
list(GET ZLINK_GENERATOR 1 _zlink_preset_generator)
set(_zlink_preset_architecture "")
list(LENGTH ZLINK_GENERATOR _zlink_generator_length)
if(_zlink_generator_length GREATER 3)
  list(GET ZLINK_GENERATOR 3 _zlink_preset_architecture)
  set(_zlink_preset_architecture "      \"architecture\": \"${_zlink_preset_architecture}\",\n")
endif()
if(CMAKE_HOST_WIN32)
  set(_zlink_preset_build
    "      \"configurePreset\": \"zlink\",\n"
    "      \"configuration\": \"${ZLINK_CONFIG}\"\n")
else()
  set(_zlink_preset_build "      \"configurePreset\": \"zlink\"\n")
endif()
file(WRITE "${ZLINK_PROJECT_DIR}/CMakeUserPresets.json"
  "{\n"
  "  \"version\": 4,\n"
  "  \"configurePresets\": [\n"
  "    {\n"
  "      \"name\": \"zlink\",\n"
  "      \"displayName\": \"ZLink installed package\",\n"
  "      \"generator\": \"${_zlink_preset_generator}\",\n"
  "${_zlink_preset_architecture}"
  "      \"binaryDir\": \"${ZLINK_PROJECT_DIR}/build\",\n"
  "      \"cacheVariables\": {\n"
  "${_zlink_preset_cache}"
  "      }\n"
  "    }\n"
  "  ],\n"
  "  \"buildPresets\": [\n"
  "    {\n"
  "      \"name\": \"zlink\",\n"
  "${_zlink_preset_build}"
  "    }\n"
  "  ]\n"
  "}\n")
message(STATUS "bootstrap complete; build with: cmake --build build ${ZLINK_BUILD_CONFIG}")
