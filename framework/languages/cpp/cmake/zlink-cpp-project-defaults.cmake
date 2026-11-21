include(FetchContent)
list(APPEND CMAKE_MODULE_PATH "${ZLINK_FRAMEWORK_CPP_DIR}/cmake")
option(ZLINK_FRAMEWORK_CPP_SHARED
  "Build distributable C++ libraries as shared libraries" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Shared libraries keep their out-of-line definitions visible; the linker
  # limits their exported surface. Static builds hide executable symbols.
  if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden>
      $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
  else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
  endif()
  add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>)
endif()

if(MSVC)
  add_compile_definitions(NOMINMAX)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

if(ZLINK_FRAMEWORK_CPP_SHARED)
  set(ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE SHARED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  if(WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
else()
  set(ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE STATIC)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(yyjson CONFIG QUIET)
if(NOT yyjson_FOUND)
  FetchContent_Declare(
    yyjson
    URL https://codeload.github.com/ibireme/yyjson/tar.gz/refs/tags/0.12.0
    URL_HASH SHA256=b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # Compile the pinned source directly; do not execute downloaded build scripts.
  FetchContent_GetProperties(yyjson)
  if(NOT yyjson_POPULATED)
    FetchContent_Populate(yyjson)
  endif()
  enable_language(C)
  add_library(yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
  set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(yyjson PUBLIC
    $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)
  add_library(yyjson::yyjson ALIAS yyjson)
  # Match the upstream package's target/config names for installed consumers.
  export(TARGETS yyjson NAMESPACE yyjson::
    FILE "${yyjson_BINARY_DIR}/yyjson-config.cmake")
  set(yyjson_DIR "${yyjson_BINARY_DIR}")
  install(TARGETS yyjson EXPORT yyjson-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  install(EXPORT yyjson-targets FILE yyjson-config.cmake
    NAMESPACE yyjson:: DESTINATION lib/cmake/yyjson)
  install(FILES "${yyjson_SOURCE_DIR}/src/yyjson.h" DESTINATION include)
  install(FILES "${yyjson_SOURCE_DIR}/LICENSE" DESTINATION share/licenses/yyjson)
endif()
