[English](./framework-cpp-0.18.3.md) | [한국어](./framework-cpp-0.18.3.ko.md)

# ZLink C++ Framework 0.18.3 Release Notes

Framework 0.18.3 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change. The framework source is identical to 0.18.2.

## Fixes

- `bootstrap.cmake` in the distributed zips (`zlink-tutorial-cpp.zip`, `zlink-samples-cpp.zip`) hard-coded the framework source archive version as `0.18.0`, so the 0.18.1 and 0.18.2 zips fetched and built 0.18.0 rather than their own release. `sync-version` now owns that value, so a zip fetches the source archive of the release tag it ships with. If you downloaded the 0.18.1 or 0.18.2 zips, replace them with this release's to get the #692 and #665 fixes. (#718)

For the 0.18.1 and 0.18.2 changes see the [0.18.1](./framework-cpp-0.18.1.md) and [0.18.2](./framework-cpp-0.18.2.md) release notes.

## Installation

Download `zlink-framework-cpp-0.18.3.tar.gz` from the [`framework-cpp/v0.18.3` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.3) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and Conan recipe are also available.
