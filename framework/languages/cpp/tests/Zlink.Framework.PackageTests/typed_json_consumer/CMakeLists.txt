cmake_minimum_required(VERSION 3.20)
project(zlink_typed_json_consumer LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)
if(ZLINK_TYPED_JSON_ENGINE STREQUAL "framework")
  find_package(zlink_framework_cpp CONFIG REQUIRED)
  set(engine_target zlink::framework)
elseif(ZLINK_TYPED_JSON_ENGINE STREQUAL "connector")
  find_package(zlink_stream_connector_cpp CONFIG REQUIRED)
  set(engine_target zlink::stream_connector)
else()
  message(FATAL_ERROR "ZLINK_TYPED_JSON_ENGINE must be framework or connector")
endif()

# Keep consumer visibility at its compiler default. Hidden visibility would
# conceal duplicated ownership between header instantiations and a shared engine.
add_executable(typed_json_consumer main.cpp)
target_compile_features(typed_json_consumer PRIVATE cxx_std_20)
target_link_libraries(typed_json_consumer PRIVATE ${engine_target} Threads::Threads)
add_test(NAME typed_json_main_exit COMMAND typed_json_consumer main)
add_test(NAME typed_json_worker_exit COMMAND typed_json_consumer worker)
