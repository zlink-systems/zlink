cmake_minimum_required(VERSION 3.20)
project(zlink_axmol_stream_connector_tests LANGUAGES CXX)

add_executable(zlink_axmol_stream_connector_test_app
  test_app.cpp)
target_compile_features(zlink_axmol_stream_connector_test_app PRIVATE cxx_std_20)
target_include_directories(zlink_axmol_stream_connector_test_app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include)
