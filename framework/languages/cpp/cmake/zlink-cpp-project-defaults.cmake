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
