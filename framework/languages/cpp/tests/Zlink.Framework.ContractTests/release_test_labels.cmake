# Tier labels classify a CTest entry once; component-prefixed labels remain areas.
set(ZLINK_FRAMEWORK_CPP_TEST_TIERS unit contract integration e2e)
set(ZLINK_FRAMEWORK_CPP_RELEASE_TEST_LABEL_REGEX "^(unit|contract)$")
