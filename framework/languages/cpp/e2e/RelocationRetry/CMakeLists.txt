cmake_minimum_required(VERSION 3.25)
project(zlink_cpp_relocation_retry_e2e LANGUAGES CXX)

find_package(zlink_framework_cpp CONFIG REQUIRED)

add_executable(zlink_cpp_e2e_relocation_retry role.cpp)
target_compile_features(zlink_cpp_e2e_relocation_retry PRIVATE cxx_std_20)
target_link_libraries(zlink_cpp_e2e_relocation_retry PRIVATE
  zlink::framework
  zlink::framework_locations_redis)
