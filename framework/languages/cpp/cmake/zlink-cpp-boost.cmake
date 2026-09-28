set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR "" CACHE PATH
  "Boost include directory used by the C++ framework and stream connector")
if(NOT ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST AND NOT ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    AND EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost/boost/asio.hpp")
  set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost" CACHE PATH
    "Boost include directory used by the C++ framework and stream connector" FORCE)
endif()
if(ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST OR NOT ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR)
  find_package(Boost REQUIRED)
endif()
function(zlink_framework_cpp_add_boost_headers target_name)
  if(NOT ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST AND ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR)
    # Keep the bundled Boost tree ahead of transitive package include paths. A
    # framework target and the standalone HTTP client are linked into the same
    # consumer; selecting different Asio headers in those targets is an ABI
    # violation even though the headers are nominally header-only.
    target_include_directories(${target_name} BEFORE PRIVATE
      "${ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR}")
  elseif(TARGET Boost::headers)
    target_link_libraries(${target_name} PRIVATE Boost::headers)
  elseif(Boost_FOUND AND Boost_INCLUDE_DIRS)
    target_include_directories(${target_name} BEFORE PRIVATE ${Boost_INCLUDE_DIRS})
  elseif(Boost_FOUND AND Boost_INCLUDE_DIR)
    target_include_directories(${target_name} BEFORE PRIVATE ${Boost_INCLUDE_DIR})
  else()
    message(FATAL_ERROR
      "Boost headers were not found. Install Boost, configure with vcpkg, "
      "or make sure core/external/boost exists in the repository checkout.")
  endif()
endfunction()

# Boost.Asio owns the socket implementation and keeps the runtime free of
# platform-specific transport branches. Windows still requires the Winsock
# libraries at final link time, including when a static connector is consumed
# by an engine module.
function(zlink_framework_cpp_add_asio_system_libraries target_name)
  if(WIN32)
    target_link_libraries(${target_name} PRIVATE ws2_32 mswsock)
  endif()
endfunction()
