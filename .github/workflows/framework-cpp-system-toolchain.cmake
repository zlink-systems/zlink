# Use the single vcpkg installation prepared and checked by the CI job.
set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "" FORCE)
set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLED_DIR}" CACHE PATH "" FORCE)
set(ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST ON CACHE BOOL "" FORCE)
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
