# SPDX-License-Identifier: Apache-2.0
# Standalone header-only test: instrument JSON value construction without changing the library.
get_target_property(_json_includes nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
find_file(_json_base_header nlohmann/detail/json_custom_base_class.hpp
  PATHS ${_json_includes} NO_DEFAULT_PATH REQUIRED)
file(READ "${_json_base_header}" _json_base)
string(FIND "${_json_base}" "struct json_default_base {};" _json_base_marker)
if(_json_base_marker EQUAL -1)
  message(FATAL_ERROR "Typed JSON allocation test requires the nlohmann default-base instrumentation point")
endif()
string(REPLACE "struct json_default_base {};" [=[
inline thread_local unsigned long long zlink_dom_constructions = 0;
struct json_default_base {
 json_default_base() noexcept { ++zlink_dom_constructions; }
 json_default_base(const json_default_base&) noexcept { ++zlink_dom_constructions; }
 json_default_base(json_default_base&&) noexcept { ++zlink_dom_constructions; }
 json_default_base& operator=(const json_default_base&) = default;
 json_default_base& operator=(json_default_base&&) = default;
};

]=] _json_instrumented "${_json_base}")
set(_json_instrument_dir "${CMAKE_CURRENT_BINARY_DIR}/typed-json-dom-instrument")
file(WRITE "${_json_instrument_dir}/nlohmann/detail/json_custom_base_class.hpp" "${_json_instrumented}")
add_executable(test_cpp_framework_typed_json_allocations
  tests/Zlink.Framework.UnitTests/test_cpp_framework_typed_json_allocations.cpp
  tests/support/allocation_counter.cpp)
target_link_libraries(test_cpp_framework_typed_json_allocations PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(test_cpp_framework_typed_json_allocations BEFORE PRIVATE
  "${_json_instrument_dir}" common/include tests/support)
target_compile_definitions(test_cpp_framework_typed_json_allocations PRIVATE ZLINK_JSON_EXPECT_DOM_FREE)
if(MSVC)
  target_compile_options(test_cpp_framework_typed_json_allocations PRIVATE /UNDEBUG)
else()
  target_compile_options(test_cpp_framework_typed_json_allocations PRIVATE -UNDEBUG)
endif()
add_test(NAME test_cpp_framework_typed_json_allocations COMMAND test_cpp_framework_typed_json_allocations)
set_tests_properties(test_cpp_framework_typed_json_allocations PROPERTIES LABELS "framework-unit;framework-regression;messaging;unit" TIMEOUT 30)
