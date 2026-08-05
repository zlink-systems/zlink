cmake_minimum_required(VERSION 3.20)
project(zlink_axmol_stream_connector LANGUAGES CXX)

add_library(zlink_axmol_stream_connector STATIC
  src/zlink_axmol_stream_connector.cpp)
target_compile_features(zlink_axmol_stream_connector PUBLIC cxx_std_20)
target_include_directories(zlink_axmol_stream_connector PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(zlink_axmol_stream_connector PRIVATE
  zlink::stream_connector
  zlink::stream_connector_codecs)
