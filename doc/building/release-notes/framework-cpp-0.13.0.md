[한국어](./framework-cpp-0.13.0.ko.md) | [English](./framework-cpp-0.13.0.md)

# ZLink C++ Framework 0.13.0 release notes

Framework 0.13.0 uses binding 1.1.0 and Core 1.1.0. Framework releases are versioned independently per language.

## Highlights

- This release exists to ship the C++ packaging fixes in a published artifact. No user-visible API changes.
- The C++ installation now works under the vcpkg and Conan layouts. `find_package(zlink_framework CONFIG REQUIRED)` failed because the installed files and what the CMake config promised had drifted apart.

## Installation

Download `zlink-framework-cpp-0.13.0.tar.gz` from the [`framework-cpp/v0.13.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.13.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and the Conan recipe also work.
