# Builds the published ZLink C++ stack next to this project, then configures
# this project against it. Run once from this directory:
#
#     cmake -P bootstrap.cmake
#
# Nothing here reads the zlink repository. The three inputs are GitHub Release
# assets -- the Core prebuilt archive for this platform (core/v<CORE>), the C++
# binding source archive (cpp/v<BINDING>) and the framework source archive
# (framework-cpp/v<FRAMEWORK>) -- and every third-party library comes from
# ConanCenter through the framework archive's Conan recipe (the default), or
# vcpkg through its vcpkg.json when explicitly selected. Only the framework
# version is pinned here; the Core and binding versions it was released
# against, and the third-party list, are read from the framework archive.
#
# Output layout (.zlink/ beside this file; delete it to start over):
#
#     .zlink/downloads/      the three release assets
#     .zlink/src/            extracted binding and framework sources
#     .zlink/core/           Core prebuilt prefix (headers, library, CMake config)
#     .zlink/cpp/            binding install prefix
#     .zlink/install/        framework install prefix; also carries the binding
#                            and Core so a consumer needs only this one prefix
#     .zlink/conan/          generated consumer recipe, profile and generators
#     .zlink/vcpkg_installed the vcpkg tree when -DZLINK_PACKAGE_MANAGER=vcpkg
#     build/                 this project, configured; build it with
#                            `cmake --build build --config Release`
#
# Options (pass as -D<name>=<value> before -P):
#     ZLINK_PACKAGE_MANAGER conan (default) or vcpkg
#     VCPKG_ROOT   vcpkg checkout when ZLINK_PACKAGE_MANAGER=vcpkg (default: $VCPKG_ROOT, $VCPKG_INSTALLATION_ROOT,
#                  then the copy bundled with Visual Studio 2022 on Windows)
#     ZLINK_JOBS   parallel compile jobs (default: logical cores)
#     ZLINK_ROOT   where .zlink/ goes (default: beside this file)
#     ZLINK_CONAN_RECIPE  conanfile.py to read the third-party list from instead
#                  of the one inside the downloaded framework archive. Only for
#                  verifying an unreleased recipe (repository CI); readers never set it
cmake_minimum_required(VERSION 3.24)

set(ZLINK_FRAMEWORK_CPP_VERSION "0.22.0")
set(ZLINK_VCPKG_BASELINE "a1cae005c39be7b18ba319fced856b68d7276271")
set(ZLINK_RELEASES "https://github.com/zlink-systems/zlink/releases/download")
set(ZLINK_PACKAGE_MANAGER "conan" CACHE STRING "Third-party package manager: conan or vcpkg")
set_property(CACHE ZLINK_PACKAGE_MANAGER PROPERTY STRINGS conan vcpkg)

set(ZLINK_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT ZLINK_ROOT)
  set(ZLINK_ROOT "${ZLINK_PROJECT_DIR}/.zlink")
endif()
if(NOT ZLINK_JOBS)
  cmake_host_system_information(RESULT ZLINK_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
endif()
file(MAKE_DIRECTORY "${ZLINK_ROOT}/downloads")

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
if(_zlink_arch STREQUAL "x64")
  set(ZLINK_CONAN_ARCH "x86_64")
elseif(_zlink_arch STREQUAL "arm64")
  set(ZLINK_CONAN_ARCH "armv8")
else()
  zlink_fail("no Conan architecture mapping for ${_zlink_arch}")
endif()

# --- vcpkg (legacy opt-in) -----------------------------------------------------
# The first candidate that really holds the toolchain wins: -DVCPKG_ROOT, the
# VCPKG_ROOT environment variable, VCPKG_INSTALLATION_ROOT (GitHub Actions
# runner images), then the copy Visual Studio 2022 installs on Windows.
# --- generator and compiler flags ------------------------------------------------
# One Release configuration end to end: the framework is a static library and
# MSVC cannot link a Debug consumer against Release objects.
set(ZLINK_CONFIG Release)
if(CMAKE_HOST_WIN32)
  # The generator and the Conan profile both describe the Visual Studio that is
  # actually installed: vswhere names the newest one with the C++ tools (2022 =
  # generator "Visual Studio 17 2022", 2026 = "Visual Studio 18 2026"), and its
  # default toolset file names the MSVC toolset, which is Conan's msvc setting
  # (14.3x -> 193, 14.4x -> 194, 14.5x -> 195). A profile that pins a toolset
  # the machine does not have makes Conan build OpenSSL from source with a
  # vcvars toolset that does not exist (#888): a fresh VS 2022 17.10+ install
  # carries 14.4x only.
  set(_zlink_vswhere "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(NOT EXISTS "${_zlink_vswhere}")
    # vswhere is a 32-bit installer component; on x64 it lives under Program Files (x86).
    set(_zlink_vswhere "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
  endif()
  if(NOT EXISTS "${_zlink_vswhere}")
    zlink_fail("could not find vswhere.exe; install Visual Studio 2022 or 2026 with the 'Desktop development with C++' workload")
  endif()
  # -prerelease: a machine whose only Visual Studio is a preview (Insiders)
  # channel still has a complete toolset; vswhere orders by version, so a
  # released 2026 wins over a 2022 either way.
  execute_process(COMMAND "${_zlink_vswhere}" -latest -prerelease -products * -version "[17.0,19.0)"
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
    -property installationPath OUTPUT_VARIABLE ZLINK_VS_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${_zlink_vswhere}" -latest -prerelease -products * -version "[17.0,19.0)"
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
    -property installationVersion OUTPUT_VARIABLE _zlink_vs_version OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT ZLINK_VS_ROOT OR NOT _zlink_vs_version MATCHES "^(17|18)\\.")
    zlink_fail("could not find Visual Studio 2022 or 2026 with the C++ tools (vswhere found none); install the 'Desktop development with C++' workload")
  endif()
  if(CMAKE_MATCH_1 STREQUAL "18")
    set(ZLINK_GENERATOR -G "Visual Studio 18 2026" -A x64)
  else()
    set(ZLINK_GENERATOR -G "Visual Studio 17 2022" -A x64)
  endif()
  set(_zlink_vc_toolset_file "${ZLINK_VS_ROOT}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")
  if(NOT EXISTS "${_zlink_vc_toolset_file}")
    zlink_fail("${ZLINK_VS_ROOT} has no default MSVC toolset (${_zlink_vc_toolset_file}); install the 'Desktop development with C++' workload")
  endif()
  file(READ "${_zlink_vc_toolset_file}" _zlink_vc_toolset)
  string(STRIP "${_zlink_vc_toolset}" _zlink_vc_toolset)
  if(NOT _zlink_vc_toolset MATCHES "^14\\.([0-9])[0-9]\\.")
    zlink_fail("unsupported MSVC toolset '${_zlink_vc_toolset}' in ${_zlink_vc_toolset_file}")
  endif()
  math(EXPR ZLINK_MSVC_VERSION "190 + ${CMAKE_MATCH_1}")
  message(STATUS "Visual Studio ${_zlink_vs_version} at ${ZLINK_VS_ROOT}: toolset ${_zlink_vc_toolset} (Conan msvc ${ZLINK_MSVC_VERSION})")
  # CMP0091 NEW: the Conan toolchain sets CMAKE_MSVC_RUNTIME_LIBRARY and refuses
  # a project whose cmake_minimum_required predates that policy (the binding's).
  set(ZLINK_COMPILER_FLAGS
    "-DCMAKE_CXX_FLAGS=/EHsc /utf-8 /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00"
    "-DCMAKE_CXX_FLAGS_RELEASE=/MD /Od /DNDEBUG"
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW)
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
    ${ZLINK_PACKAGE_MANAGER_ARGS} ${ZLINK_COMPILER_FLAGS}
    -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
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

# Every build below, this project included, resolves third-party libraries from
# one package-manager tree. Conan is the normal path: its generated consumer
# uses the framework recipe's third-party requirements, but deliberately omits
# zlink-cpp because this script builds the release binding archive below.
if(NOT ZLINK_PACKAGE_MANAGER MATCHES "^(conan|vcpkg)$")
  zlink_fail("ZLINK_PACKAGE_MANAGER must be conan or vcpkg, not '${ZLINK_PACKAGE_MANAGER}'")
endif()
if(ZLINK_PACKAGE_MANAGER STREQUAL "conan")
  find_program(ZLINK_CONAN conan REQUIRED)
  set(ZLINK_CONAN_DIR "${ZLINK_ROOT}/conan")
  set(ZLINK_CONAN_PROFILE "${ZLINK_CONAN_DIR}/profiles/zlink-bootstrap")
  set(ZLINK_CONAN_GENERATORS "${ZLINK_CONAN_DIR}/generators")
  file(MAKE_DIRECTORY "${ZLINK_CONAN_DIR}/profiles" "${ZLINK_CONAN_GENERATORS}")

  # The released recipe is the version owner. Read its requirement literals so
  # a bootstrap always follows the archive it downloaded, not this script.
  set(_zlink_recipe "${ZLINK_FRAMEWORK_SRC}/packaging/conan/conanfile.py")
  if(ZLINK_CONAN_RECIPE)
    set(_zlink_recipe "${ZLINK_CONAN_RECIPE}")
  endif()
  if(NOT EXISTS "${_zlink_recipe}")
    zlink_fail("the framework archive has no packaging/conan/conanfile.py")
  endif()
  file(READ "${_zlink_recipe}" _zlink_recipe_text)
  string(REPLACE "\n" ";" _zlink_recipe_lines "${_zlink_recipe_text}")
  set(_zlink_third_party_requirements "")
  set(_zlink_in_owner_list OFF)
  foreach(_zlink_recipe_line IN LISTS _zlink_recipe_lines)
    if(_zlink_recipe_line MATCHES "^ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS = \\(")
      set(_zlink_in_owner_list ON)
    elseif(_zlink_in_owner_list AND _zlink_recipe_line MATCHES "^\\)")
      set(_zlink_in_owner_list OFF)
    elseif(_zlink_in_owner_list)
      string(REGEX MATCH "\"([A-Za-z0-9_.+-]+/[^\"]+)\""
        _zlink_quoted_requirement "${_zlink_recipe_line}")
      if(_zlink_quoted_requirement)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1"
          _zlink_requirement "${_zlink_quoted_requirement}")
        list(APPEND _zlink_third_party_requirements "${_zlink_requirement}")
      endif()
    endif()
  endforeach()
  # Framework archives published before the owner tuple used literal entries
  # in requirements(); retain that reader so the current release remains
  # bootstrapable during this transition.
  if(NOT _zlink_third_party_requirements)
    set(_zlink_in_requirements OFF)
    foreach(_zlink_recipe_line IN LISTS _zlink_recipe_lines)
      if(_zlink_recipe_line MATCHES "^    def requirements\\(self\\):")
        set(_zlink_in_requirements ON)
      elseif(_zlink_in_requirements AND _zlink_recipe_line MATCHES "^    def ")
        set(_zlink_in_requirements OFF)
      elseif(_zlink_in_requirements)
        string(REGEX MATCH "\"([A-Za-z0-9_.+-]+/[^\"]+)\""
          _zlink_quoted_requirement "${_zlink_recipe_line}")
        if(_zlink_quoted_requirement)
          string(REGEX REPLACE "^\"(.*)\"$" "\\1"
            _zlink_requirement "${_zlink_quoted_requirement}")
          # redis-plus-plus async owns libuv's compatible version. Older
          # release recipes listed libuv directly, which conflicts with it.
          if(NOT _zlink_requirement MATCHES "^(zlink(-cpp)?|libuv)/")
            list(APPEND _zlink_third_party_requirements "${_zlink_requirement}")
          endif()
        endif()
      endif()
    endforeach()
  endif()
  list(REMOVE_DUPLICATES _zlink_third_party_requirements)
  list(FIND _zlink_third_party_requirements "opentelemetry-cpp/1.26.0"
    _zlink_otel_requirement)
  if(NOT _zlink_third_party_requirements OR _zlink_otel_requirement EQUAL -1)
    zlink_fail("could not read third-party requirements from ${_zlink_recipe}")
  endif()
  set(_zlink_conanfile "from conan import ConanFile\n\nclass ZlinkBootstrap(ConanFile):\n    name = \"zlink-bootstrap\"\n    version = \"0\"\n    settings = \"os\", \"arch\", \"compiler\", \"build_type\"\n    default_options = {\"boost/*:header_only\": True, \"redis-plus-plus/*:build_async\": True}\n    generators = \"CMakeDeps\", \"CMakeToolchain\"\n\n    def requirements(self):\n        for requirement in (\n")
  foreach(_zlink_requirement IN LISTS _zlink_third_party_requirements)
    string(APPEND _zlink_conanfile "            \"${_zlink_requirement}\",\n")
  endforeach()
  string(APPEND _zlink_conanfile "        ):\n            self.requires(requirement, transitive_headers=True, transitive_libs=True)\n")
  file(WRITE "${ZLINK_CONAN_DIR}/conanfile.py" "${_zlink_conanfile}")

  if(CMAKE_HOST_WIN32)
    # /MD matches the Release flags below and keeps all C++ objects on the dynamic CRT.
    set(_zlink_conan_settings "os=Windows\narch=x86_64\nbuild_type=Release\ncompiler=msvc\ncompiler.version=${ZLINK_MSVC_VERSION}\ncompiler.runtime=dynamic\ncompiler.cppstd=20")
    set(_zlink_dep_cppstd "17")
    set(_zlink_consumer_cppstd "20")
  elseif(CMAKE_HOST_APPLE)
    execute_process(COMMAND xcrun --find clang OUTPUT_VARIABLE _zlink_clang OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _zlink_clang_status)
    if(NOT _zlink_clang_status EQUAL 0)
      zlink_fail("could not find Apple clang for the Conan profile")
    endif()
    execute_process(COMMAND "${_zlink_clang}" --version OUTPUT_VARIABLE _zlink_clang_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "Apple clang version ([0-9]+)" _zlink_clang_match "${_zlink_clang_version}")
    if(NOT _zlink_clang_match)
      zlink_fail("could not determine Apple clang version for the Conan profile")
    endif()
    set(_zlink_conan_settings "os=Macos\narch=${ZLINK_CONAN_ARCH}\nbuild_type=Release\ncompiler=apple-clang\ncompiler.version=${CMAKE_MATCH_1}\ncompiler.libcxx=libc++\ncompiler.cppstd=gnu20")
    set(_zlink_dep_cppstd "gnu17")
    set(_zlink_consumer_cppstd "gnu20")
  else()
    find_program(_zlink_gxx NAMES g++-13 g++ REQUIRED)
    execute_process(COMMAND "${_zlink_gxx}" -dumpfullversion -dumpversion OUTPUT_VARIABLE _zlink_gxx_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^[0-9]+" _zlink_gxx_match "${_zlink_gxx_version}")
    if(NOT _zlink_gxx_match)
      zlink_fail("could not determine GCC version for the Conan profile")
    endif()
    set(_zlink_conan_settings "os=Linux\narch=${ZLINK_CONAN_ARCH}\nbuild_type=Release\ncompiler=gcc\ncompiler.version=${CMAKE_MATCH_0}\ncompiler.libcxx=libstdc++11\ncompiler.cppstd=gnu20")
    set(_zlink_dep_cppstd "gnu17")
    set(_zlink_consumer_cppstd "gnu20")
  endif()
  file(WRITE "${ZLINK_CONAN_PROFILE}" "[settings]\n${_zlink_conan_settings}\n*:compiler.cppstd=${_zlink_dep_cppstd}\nzlink-bootstrap/*:compiler.cppstd=${_zlink_consumer_cppstd}\n")
  message(STATUS "Conan third-party requirements: ${_zlink_third_party_requirements}")
  message(STATUS "Conan profile: dependencies ${_zlink_dep_cppstd}; bootstrap consumer ${_zlink_consumer_cppstd}")
  zlink_run("${ZLINK_CONAN}" install "${ZLINK_CONAN_DIR}"
    "--profile:host=${ZLINK_CONAN_PROFILE}" "--profile:build=${ZLINK_CONAN_PROFILE}"
    --build=missing --output-folder "${ZLINK_CONAN_GENERATORS}")
  set(ZLINK_PACKAGE_MANAGER_ARGS
    "-DCMAKE_TOOLCHAIN_FILE=${ZLINK_CONAN_GENERATORS}/conan_toolchain.cmake")
else()
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
  if(NOT VCPKG_ROOT)
    zlink_fail("vcpkg was not found. Set VCPKG_ROOT to a vcpkg checkout "
      "(https://github.com/microsoft/vcpkg) or pass -DVCPKG_ROOT=<dir>")
  endif()
  set(ZLINK_TOOLCHAIN "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
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
  file(MAKE_DIRECTORY "${ZLINK_MANIFEST_DIR}")
  file(WRITE "${ZLINK_MANIFEST_DIR}/vcpkg.json" "${_zlink_manifest}\n")
  set(ZLINK_PACKAGE_MANAGER_ARGS
    "-DCMAKE_TOOLCHAIN_FILE=${ZLINK_TOOLCHAIN}"
    "-DVCPKG_MANIFEST_DIR=${ZLINK_MANIFEST_DIR}"
    "-DVCPKG_INSTALLED_DIR=${ZLINK_ROOT}/vcpkg_installed")
endif()

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
  ${ZLINK_GENERATOR} ${ZLINK_PACKAGE_MANAGER_ARGS} ${ZLINK_COMPILER_FLAGS}
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
  "-DCMAKE_PREFIX_PATH=${ZLINK_INSTALL_PREFIX}")
message(STATUS "bootstrap done. Next: cmake --build \"${ZLINK_PROJECT_DIR}/build\" "
  "--config ${ZLINK_CONFIG} --parallel ${ZLINK_JOBS}")
