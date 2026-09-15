if(NOT DEFINED ZLINK_FRAMEWORK_CPP_SOURCE_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_SOURCE_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()

set(presets_file "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/CMakePresets.json")
set(vcpkg_file "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}/vcpkg.json")

if(NOT EXISTS "${presets_file}")
  message(FATAL_ERROR "CMakePresets.json is required")
endif()
if(NOT EXISTS "${vcpkg_file}")
  message(FATAL_ERROR "vcpkg.json is required")
endif()

file(READ "${presets_file}" presets_text)
file(READ "${vcpkg_file}" vcpkg_text)

foreach(required
    "\"linux-ninja-debug\""
    "\"linux-ninja-release\""
    "\"linux-ninja-vcpkg-debug\""
    "\"macos-ninja-debug\""
    "\"windows-msvc-debug\""
    "\"windows-msvc-release\""
    "\"windows-msvc-vcpkg-debug\""
    "\"Visual Studio 17 2022\""
    "\"CMAKE_CXX_STANDARD\": \"20\""
    "\"CMAKE_EXPORT_COMPILE_COMMANDS\": \"ON\""
    "\"ZLINK_FRAMEWORK_CPP_BUILD_TESTS\": \"ON\""
    "\"ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES\": \"ON\"")
  if(NOT presets_text MATCHES "${required}")
    message(FATAL_ERROR "CMakePresets.json is missing ${required}")
  endif()
endforeach()

foreach(required
    "\"boost-asio\""
    "\"gtest\""
    "\"lz4\""
    "\"nlohmann-json\""
    "\"openssl\"")
  if(NOT vcpkg_text MATCHES "${required}")
    message(FATAL_ERROR "vcpkg.json is missing ${required}")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --list-presets=all
  WORKING_DIRECTORY "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}"
  RESULT_VARIABLE list_result
  OUTPUT_VARIABLE list_output
  ERROR_VARIABLE list_error)
if(NOT list_result EQUAL 0)
  message(FATAL_ERROR "cmake --list-presets=all failed: ${list_error}")
endif()

foreach(required
    "linux-ninja-debug"
    "linux-ninja-release"
    "linux-ninja-vcpkg-debug"
    "macos-ninja-debug"
    "windows-msvc-debug"
    "windows-msvc-release"
    "windows-msvc-vcpkg-debug")
  if(NOT list_output MATCHES "${required}")
    message(FATAL_ERROR "cmake --list-presets=all did not list ${required}")
  endif()
endforeach()

set(smoke_generator_args -G Ninja)
set(smoke_build_name linux-ninja-debug)
if(WIN32)
  if(NOT DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR)
    message(FATAL_ERROR "The Windows tooling generator is required")
  endif()
  set(smoke_generator_args -G "${ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR}")
  if(DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR_PLATFORM
      AND NOT ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND smoke_generator_args
      -A "${ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR_PLATFORM}")
  endif()
  set(smoke_build_name windows-msvc-debug)
else()
  find_program(ZLINK_NINJA_EXECUTABLE ninja)
  if(NOT ZLINK_NINJA_EXECUTABLE)
    message(FATAL_ERROR "Ninja is required for CLion-style configure smoke")
  endif()
  list(APPEND smoke_generator_args
    -D CMAKE_MAKE_PROGRAM=${ZLINK_NINJA_EXECUTABLE})
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef tooling_run_id)
set(smoke_run_dir
  "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/tooling-smoke-runs/${tooling_run_id}")
set(smoke_build_dir
  "${smoke_run_dir}/${smoke_build_name}")
set(tooling_configure_args)
if(DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_CMAKE_TOOLCHAIN_FILE)
  list(APPEND tooling_configure_args
    -D CMAKE_TOOLCHAIN_FILE=${ZLINK_FRAMEWORK_CPP_TOOLING_CMAKE_TOOLCHAIN_FILE})
endif()
foreach(tooling_dependency IN ITEMS
    protobuf_DIR
    absl_DIR
    utf8_range_DIR
    ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX
    ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX)
  if(DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_${tooling_dependency}
      AND NOT "${ZLINK_FRAMEWORK_CPP_TOOLING_${tooling_dependency}}" STREQUAL "")
    list(APPEND tooling_configure_args
      -D ${tooling_dependency}=${ZLINK_FRAMEWORK_CPP_TOOLING_${tooling_dependency}})
  endif()
endforeach()
if(DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_INSTALLED_DIR)
  list(APPEND tooling_configure_args
    -D VCPKG_MANIFEST_MODE=OFF
    -D VCPKG_INSTALLED_DIR=${ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_INSTALLED_DIR})
endif()
if(DEFINED ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_TARGET_TRIPLET)
  list(APPEND tooling_configure_args
    -D VCPKG_TARGET_TRIPLET=${ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_TARGET_TRIPLET})
endif()
file(MAKE_DIRECTORY "${smoke_run_dir}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${ZLINK_FRAMEWORK_CPP_SOURCE_DIR}"
    -B "${smoke_build_dir}"
    ${smoke_generator_args}
    -D CMAKE_BUILD_TYPE=Debug
    -D CMAKE_CXX_STANDARD=20
    -D CMAKE_CXX_EXTENSIONS=OFF
    -D CMAKE_EXPORT_COMPILE_COMMANDS=ON
    -D ZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON
    -D ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=ON
    ${tooling_configure_args}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(STATUS "configure stdout:\n${configure_output}")
  message(STATUS "configure stderr:\n${configure_error}")
  message(FATAL_ERROR "tooling configure smoke failed")
endif()

if(WIN32 AND NOT EXISTS "${smoke_build_dir}/zlink_framework_cpp.sln")
  message(FATAL_ERROR
    "Visual Studio tooling configure smoke did not produce zlink_framework_cpp.sln")
elseif(NOT WIN32 AND NOT EXISTS "${smoke_build_dir}/compile_commands.json")
  message(FATAL_ERROR
    "Ninja tooling configure smoke did not produce compile_commands.json")
endif()

file(READ "${smoke_build_dir}/CMakeCache.txt" smoke_cache)
foreach(required
    "ZLINK_FRAMEWORK_CPP_BUILD_TESTS:BOOL=ON"
    "ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES:BOOL=ON")
  if(NOT smoke_cache MATCHES "${required}")
    message(FATAL_ERROR "tooling configure cache is missing ${required}")
  endif()
endforeach()
if(NOT smoke_cache MATCHES "CMAKE_EXPORT_COMPILE_COMMANDS:[^=]*=ON")
  message(FATAL_ERROR
    "CLion-style configure cache is missing CMAKE_EXPORT_COMPILE_COMMANDS=ON")
endif()
