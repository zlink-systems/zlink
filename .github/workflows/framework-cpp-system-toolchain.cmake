# Keep the framework's vcpkg-aware configure contract without allowing its
# manifest to rebuild dependencies that the CI job installs from Ubuntu.
set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "" FORCE)
set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLED_DIR}" CACHE PATH "" FORCE)
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
