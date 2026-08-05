# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET zlink_c)
  message(FATAL_ERROR "C binding target not found. Build bindings/c first.")
endif()

get_filename_component(ZLINK_C_BINDINGS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(ZLINK_REPO_ROOT_DIR "${ZLINK_C_BINDINGS_DIR}/../.." ABSOLUTE)

set(ZLINK_C_BINDINGS_INCLUDE_DIR "${ZLINK_C_BINDINGS_DIR}/include")

function(add_c_sample target_name source_file)
  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name} PRIVATE zlink_c)
  target_include_directories(${target_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ZLINK_C_BINDINGS_INCLUDE_DIR})
  if(UNIX AND NOT WIN32)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
  elseif(APPLE)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
  endif()
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
  set_target_properties(${target_name} PROPERTIES C_STANDARD 11)
  if(UNIX AND NOT WIN32)
    target_link_libraries(${target_name} PRIVATE pthread)
  endif()
  if(ZLINK_C_REGISTER_CTEST)
    if(UNIX AND NOT WIN32)
      add_test(NAME sample_smoke_${target_name}
        COMMAND /bin/bash -lc "$<TARGET_FILE:${target_name}>")
    else()
      add_test(NAME sample_smoke_${target_name} COMMAND ${target_name})
    endif()
    set_tests_properties(sample_smoke_${target_name} PROPERTIES
      LABELS "sample-smoke"
      TIMEOUT 120
      WORKING_DIRECTORY ${ZLINK_REPO_ROOT_DIR}
    )
    if(WIN32)
      set_tests_properties(sample_smoke_${target_name} PROPERTIES
        ENVIRONMENT "PATH=${ZLINK_C_CORE_RUNTIME_DIR};$ENV{PATH}")
    endif()
  endif()
endfunction()

add_c_sample(
  sample_c_pair_recv_sample
  pair_recv_sample.c
)
add_c_sample(
  sample_c_pubsub_recv_sample
  pubsub_recv_sample.c
)
add_c_sample(
  sample_c_dealer_router_recv_sample
  dealer_router_recv_sample.c
)
add_c_sample(
  sample_c_stream_recv_sample
  stream_recv_sample.c
)
add_c_sample(
  sample_c_stream_packet_callback_sample
  stream_packet_callback_sample.c
)
add_c_sample(
  sample_c_monitor_recv_sample
  monitor_recv_sample.c
)
