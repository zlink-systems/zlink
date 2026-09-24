add_executable(test_godot_stream_connector test_adapter.cpp)
target_compile_features(test_godot_stream_connector PRIVATE cxx_std_20)
target_link_libraries(test_godot_stream_connector PRIVATE
  zlink_godot_stream_connector
  zlink::stream_connector
  zlink::stream_connector_codecs
  zlink::cpp)
zlink_framework_cpp_add_test(
  NAME test_godot_stream_connector
  COMMAND bash
    ${CMAKE_CURRENT_LIST_DIR}/run_engine_lobby.sh
    $<TARGET_FILE:test_godot_stream_connector>
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../engines/Server
  LABELS "connector-godot-contract;connector-godot-e2e"
  TIER integration
  TIMEOUT 180
  PROPERTIES
    RESOURCE_LOCK zlink_engine_lobby)
