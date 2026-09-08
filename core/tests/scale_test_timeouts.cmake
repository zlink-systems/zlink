# The macOS CI runners execute these socket tests roughly three times slower
# than the Linux ones (measured on macos-15 vs ubuntu-24.04 for the same
# commit: test_endpoint_release 3.8 s -> 15 s, unittest_request_timeout_
# scheduler 5.9 s -> 13.8 s), so the per-test ctest deadlines leave no margin
# there and healthy tests are killed. Scale every deadline registered in the
# including directory on Apple platforms only; the tests' own internal waits
# and expectations are unchanged, and no other platform sees a different
# number. Include this once per directory that registers tests.
if(APPLE)
  set(ZLINK_TEST_TIMEOUT_SCALE 3)
  get_property(zlink_timeout_tests DIRECTORY PROPERTY TESTS)
  foreach(test_name ${zlink_timeout_tests})
    get_test_property(${test_name} TIMEOUT test_timeout)
    if(NOT test_timeout OR test_timeout STREQUAL "NOTFOUND")
      set(test_timeout 1500) # ctest's own default
    endif()
    math(EXPR test_timeout "${test_timeout} * ${ZLINK_TEST_TIMEOUT_SCALE}")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT ${test_timeout})
  endforeach()
endif()
