# Builds the published ZLink C++ stack next to this project, then configures
# this project against it. Run once from this directory:
#
#     cmake -P bootstrap.cmake
#
# Nothing here reads the zlink repository. The three inputs are GitHub Release
# assets -- the Core prebuilt archive for this platform (core/v<CORE>), the C++
# binding source archive (cpp/v<BINDING>) and the framework source archive
# (framework-cpp/v<FRAMEWORK>) -- and every third-party library comes from
# vcpkg through the framework archive's own vcpkg.json. Only the framework
# version is pinned here; the Core and binding versions it was released
# against, and the third-party list, are read from the framework archive
# itself, which is their single owner.
#
# Output layout (.zlink/ beside this file; delete it to start over):
#
#     .zlink/downloads/      the three release assets
#     .zlink/src/            extracted binding and framework sources
#     .zlink/core/           Core prebuilt prefix (headers, library, CMake config)
#     .zlink/cpp/            binding install prefix
#     .zlink/install/        framework install prefix; also carries the binding
#                            and Core so a consumer needs only this one prefix
#     .zlink/vcpkg_installed the vcpkg tree shared by every build below
#     build/                 this project, configured; build it with
#                            `cmake --build build --config Release`
#
# Options (pass as -D<name>=<value> before -P):
#     VCPKG_ROOT   vcpkg checkout (default: $VCPKG_ROOT, $VCPKG_INSTALLATION_ROOT,
#                  then the copy bundled with Visual Studio 2022 on Windows)
#     ZLINK_JOBS   parallel compile jobs (default: logical cores)
#     ZLINK_ROOT   where .zlink/ goes (default: beside this file)
cmake_minimum_required(VERSION 3.24)

set(ZLINK_FRAMEWORK_CPP_VERSION "0.18.3")
set(ZLINK_VCPKG_BASELINE "a1cae005c39be7b18ba319fced856b68d7276271")
set(ZLINK_RELEASES "https://github.com/zlink-systems/zlink/releases/download")

set(ZLINK_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT ZLINK_ROOT)
  set(ZLINK_ROOT "${ZLINK_PROJECT_DIR}/.zlink")
endif()
if(NOT ZLINK_JOBS)
  cmake_host_system_information(RESULT ZLINK_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
endif()

function(zlink_fail)
  message(FATAL_ERROR "bootstrap: " ${ARGN})
endfunction()

# --- host platform -> Core prebuilt archive name ------------------------------
cmake_host_system_information(RESULT _zlink_arch QUERY OS_PLATFORM)
string(TOLOWER "${_zlink_arch}" _zlink_arch)
if(_zlink_arch MATCHES "^(x86_64|amd64)$")
  set(_zlink_arch "x64")
elseif(_zlink_arch MATCHES "^(arm64|aarch64)$")
  set(_zlink_arch "arm64")
endif()
if(CMAKE_HOST_WIN32)
  set(ZLINK_HOST_OS "windows")
elseif(CMAKE_HOST_APPLE)
  set(ZLINK_HOST_OS "macos")
else()
  set(ZLINK_HOST_OS "linux")
endif()
set(ZLINK_CORE_PLATFORM "${ZLINK_HOST_OS}-${_zlink_arch}")
if(NOT ZLINK_CORE_PLATFORM MATCHES "^(windows-x64|linux-x64|linux-arm64|macos-arm64)$")
  zlink_fail("no prebuilt Core archive for ${ZLINK_CORE_PLATFORM}; "
    "the Core release ships windows-x64, linux-x64, linux-arm64 and macos-arm64")
endif()

# --- vcpkg ---------------------------------------------------------------------
# The first candidate that really holds the toolchain wins: -DVCPKG_ROOT, the
# VCPKG_ROOT environment variable, VCPKG_INSTALLATION_ROOT (GitHub Actions
# runner images), then the copy Visual Studio 2022 installs on Windows.
set(_zlink_vcpkg_candidates "${VCPKG_ROOT}" "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
if(CMAKE_HOST_WIN32)
  file(GLOB _zlink_vs_vcpkg
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
  foreach(_zlink_vs_toolchain IN LISTS _zlink_vs_vcpkg)
    get_filename_component(_zlink_vs_root "${_zlink_vs_toolchain}/../../.." ABSOLUTE)
    list(APPEND _zlink_vcpkg_candidates "${_zlink_vs_root}")
  endforeach()
endif()
set(VCPKG_ROOT "")
foreach(_zlink_candidate IN LISTS _zlink_vcpkg_candidates)
  if(_zlink_candidate AND EXISTS "${_zlink_candidate}/scripts/buildsystems/vcpkg.cmake")
    set(VCPKG_ROOT "${_zlink_candidate}")
    break()
  endif()
endforeach()
set(ZLINK_TOOLCHAIN "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
if(NOT VCPKG_ROOT)
  zlink_fail("vcpkg was not found. Set VCPKG_ROOT to a vcpkg checkout "
    "(https://github.com/microsoft/vcpkg) or pass -DVCPKG_ROOT=<dir>")
endif()

# --- generator and compiler flags ------------------------------------------------
# One Release configuration end to end: the framework is a static library and
# MSVC cannot link a Debug consumer against Release objects.
set(ZLINK_CONFIG Release)
if(CMAKE_HOST_WIN32)
  set(ZLINK_GENERATOR -G "Visual Studio 17 2022" -A x64)
  set(ZLINK_COMPILER_FLAGS
    "-DCMAKE_CXX_FLAGS=/EHsc /utf-8 /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00"
    "-DCMAKE_CXX_FLAGS_RELEASE=/MD /Od /DNDEBUG")
  set(ZLINK_BUILD_CONFIG --config ${ZLINK_CONFIG})
else()
  find_program(_zlink_ninja ninja)
  if(_zlink_ninja)
    set(ZLINK_GENERATOR -G Ninja)
  else()
    # Do not leave this to CMake's default: a runner can advertise Ninja as
    # its default generator without having the executable installed.
    set(ZLINK_GENERATOR -G "Unix Makefiles")
  endif()
  set(ZLINK_COMPILER_FLAGS "-DCMAKE_BUILD_TYPE=${ZLINK_CONFIG}")
  set(ZLINK_BUILD_CONFIG "")
endif()
# --- helpers -------------------------------------------------------------------
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
    zlink_fail("download failed: ${url} (${text})")
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

function(zlink_require_vcpkg_baseline baseline)
  execute_process(
    COMMAND git -C "${VCPKG_ROOT}" cat-file -e "${baseline}:versions/baseline.json"
    RESULT_VARIABLE _zlink_baseline_status
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT _zlink_baseline_status EQUAL 0)
    message(STATUS "fetching vcpkg baseline ${baseline}")
    zlink_run(git -C "${VCPKG_ROOT}" fetch --depth=1 origin "${baseline}")
    execute_process(
      COMMAND git -C "${VCPKG_ROOT}" cat-file -e "${baseline}:versions/baseline.json"
      RESULT_VARIABLE _zlink_baseline_status
      OUTPUT_QUIET ERROR_QUIET)
    if(NOT _zlink_baseline_status EQUAL 0)
      zlink_fail("vcpkg does not contain baseline ${baseline} after fetching it")
    endif()
  endif()
endfunction()

# Configure, build and install one CMake source tree.
function(zlink_build name source install)
  set(build "${ZLINK_ROOT}/build/${name}")
  message(STATUS "== ${name}: ${source}")
  zlink_run("${CMAKE_COMMAND}" -S "${source}" -B "${build}" ${ZLINK_GENERATOR}
    ${ZLINK_VCPKG_ARGS} ${ZLINK_COMPILER_FLAGS}
    "-DCMAKE_INSTALL_PREFIX=${install}" ${ARGN})
  zlink_run("${CMAKE_COMMAND}" --build "${build}" ${ZLINK_BUILD_CONFIG}
    --parallel ${ZLINK_JOBS})
  zlink_run("${CMAKE_COMMAND}" --install "${build}" ${ZLINK_BUILD_CONFIG})
endfunction()

# --- 1. framework source: pins the Core and binding versions ------------------------
set(_zlink_framework_archive "zlink-framework-cpp-${ZLINK_FRAMEWORK_CPP_VERSION}.tar.gz")
zlink_download("${_zlink_framework_archive}"
  "${ZLINK_RELEASES}/framework-cpp/v${ZLINK_FRAMEWORK_CPP_VERSION}/${_zlink_framework_archive}")
set(ZLINK_FRAMEWORK_SRC "${ZLINK_ROOT}/src/framework")
zlink_extract("${_zlink_framework_archive}" "${ZLINK_FRAMEWORK_SRC}")
file(READ "${ZLINK_FRAMEWORK_SRC}/CMakeLists.txt" _zlink_framework_cmake)
foreach(pair "CPP;ZLINK_CPP_VERSION" "CORE;ZLINK_CORE_VERSION")
  list(GET pair 0 short)
  list(GET pair 1 variable)
  if(NOT _zlink_framework_cmake MATCHES
      "set\\(ZLINK_FRAMEWORK_CPP_${variable} \"([0-9]+\\.[0-9]+\\.[0-9]+)\" CACHE STRING")
    zlink_fail("the framework archive does not declare ZLINK_FRAMEWORK_CPP_${variable}")
  endif()
  set(ZLINK_${short}_VERSION "${CMAKE_MATCH_1}")
endforeach()
message(STATUS "framework ${ZLINK_FRAMEWORK_CPP_VERSION} "
  "on binding ${ZLINK_CPP_VERSION} on Core ${ZLINK_CORE_VERSION} (${ZLINK_CORE_PLATFORM})")
# Every build below, this project included, resolves third-party libraries
# from one vcpkg tree filled from the framework's manifest. The manifest gets
# a builtin-baseline: the vcpkg bundled with Visual Studio refuses to resolve
# ports without one, and it pins the third-party versions the framework
# release was verified with. Runner-image vcpkg checkouts can be shallow, so
# make the pinned commit available before CMake invokes the toolchain.
file(READ "${ZLINK_FRAMEWORK_SRC}/vcpkg.json" _zlink_manifest)
string(JSON _zlink_has_baseline ERROR_VARIABLE _zlink_no_baseline
  GET "${_zlink_manifest}" builtin-baseline)
if(_zlink_no_baseline)
  string(JSON _zlink_manifest SET "${_zlink_manifest}"
    builtin-baseline "\"${ZLINK_VCPKG_BASELINE}\"")
endif()
string(JSON _zlink_manifest_baseline GET "${_zlink_manifest}" builtin-baseline)
zlink_require_vcpkg_baseline("${_zlink_manifest_baseline}")
set(ZLINK_MANIFEST_DIR "${ZLINK_ROOT}/manifest")
file(WRITE "${ZLINK_MANIFEST_DIR}/vcpkg.json" "${_zlink_manifest}\n")
set(ZLINK_VCPKG_ARGS
  "-DCMAKE_TOOLCHAIN_FILE=${ZLINK_TOOLCHAIN}"
  "-DVCPKG_MANIFEST_DIR=${ZLINK_MANIFEST_DIR}"
  "-DVCPKG_INSTALLED_DIR=${ZLINK_ROOT}/vcpkg_installed")

# --- 2. Core prebuilt prefix ---------------------------------------------------
set(_zlink_core_archive "libzlink-${ZLINK_CORE_PLATFORM}.tar.gz")
zlink_download("${_zlink_core_archive}"
  "${ZLINK_RELEASES}/core/v${ZLINK_CORE_VERSION}/${_zlink_core_archive}")
set(ZLINK_CORE_PREFIX "${ZLINK_ROOT}/core")
zlink_extract("${_zlink_core_archive}" "${ZLINK_CORE_PREFIX}")
if(NOT EXISTS "${ZLINK_CORE_PREFIX}/lib/cmake/zlink/zlinkConfig.cmake")
  zlink_fail("the Core archive has no lib/cmake/zlink/zlinkConfig.cmake")
endif()

# --- 3. C++ binding ------------------------------------------------------------
set(_zlink_cpp_archive "zlink-cpp-${ZLINK_CPP_VERSION}.tar.gz")
zlink_download("${_zlink_cpp_archive}"
  "${ZLINK_RELEASES}/cpp/v${ZLINK_CPP_VERSION}/${_zlink_cpp_archive}")
set(ZLINK_CPP_SRC "${ZLINK_ROOT}/src/cpp")
zlink_extract("${_zlink_cpp_archive}" "${ZLINK_CPP_SRC}")
set(ZLINK_CPP_PREFIX "${ZLINK_ROOT}/cpp")
zlink_build(cpp "${ZLINK_CPP_SRC}" "${ZLINK_CPP_PREFIX}"
  "-DZLINK_CPP_CORE_PACKAGE_PREFIX=${ZLINK_CORE_PREFIX}"
  -DZLINK_CPP_BUILD_TESTS=OFF -DZLINK_CPP_BUILD_SAMPLES=OFF -DZLINK_CPP_BUILD_BENCHMARKS=OFF)

# --- 4. framework ----------------------------------------------------------------
# The install step stages the binding and Core (library, headers, CMake config,
# zlink.dll on Windows) into the same prefix, so consumers point at it alone.
set(ZLINK_INSTALL_PREFIX "${ZLINK_ROOT}/install")
zlink_build(framework "${ZLINK_FRAMEWORK_SRC}" "${ZLINK_INSTALL_PREFIX}"
  "-DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=${ZLINK_CPP_PREFIX}"
  "-DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${ZLINK_CORE_PREFIX}"
  "-DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=${ZLINK_CPP_VERSION}"
  "-DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=${ZLINK_CORE_VERSION}"
  -DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST=ON
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF
  -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
  -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=ON
  -DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF
  -DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF
  -DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF)

# --- 5. this project -------------------------------------------------------------
message(STATUS "== configuring ${ZLINK_PROJECT_DIR}")
zlink_run("${CMAKE_COMMAND}" -S "${ZLINK_PROJECT_DIR}" -B "${ZLINK_PROJECT_DIR}/build"
  ${ZLINK_GENERATOR} ${ZLINK_VCPKG_ARGS} ${ZLINK_COMPILER_FLAGS}
  "-DCMAKE_PREFIX_PATH=${ZLINK_INSTALL_PREFIX}")
message(STATUS "bootstrap done. Next: cmake --build \"${ZLINK_PROJECT_DIR}/build\" "
  "--config ${ZLINK_CONFIG} --parallel ${ZLINK_JOBS}")
