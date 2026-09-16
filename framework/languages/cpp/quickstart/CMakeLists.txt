cmake_minimum_required(VERSION 3.20)
project(zlink_quickstart_cpp LANGUAGES CXX)

find_package(zlink_framework CONFIG REQUIRED)

add_executable(quickstart_server Server/main.cpp)
target_link_libraries(quickstart_server PRIVATE zlink::framework)
target_compile_features(quickstart_server PRIVATE cxx_std_20)

add_executable(quickstart_client Client/main.cpp)
target_link_libraries(quickstart_client PRIVATE zlink::framework)
target_compile_features(quickstart_client PRIVATE cxx_std_20)
