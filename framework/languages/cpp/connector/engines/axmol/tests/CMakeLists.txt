add_executable(zlink_axmol_stream_connector_test_app
  test_app.cpp)
target_compile_features(zlink_axmol_stream_connector_test_app PRIVATE cxx_std_20)
target_include_directories(zlink_axmol_stream_connector_test_app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(zlink_axmol_stream_connector_test_app PRIVATE
  zlink_axmol_stream_connector
  nlohmann_json::nlohmann_json)
zlink_framework_cpp_add_test(
  NAME test_axmol_stream_connector
  COMMAND bash
    ${CMAKE_CURRENT_LIST_DIR}/run_engine_lobby.sh
    $<TARGET_FILE:zlink_axmol_stream_connector_test_app>
    ${CMAKE_CURRENT_LIST_DIR}/../../../../../engines/Server
  LABELS "connector-axmol-contract;connector-axmol-e2e"
  TIER integration
  TIMEOUT 180
  PROPERTIES
    RESOURCE_LOCK zlink_engine_lobby)
