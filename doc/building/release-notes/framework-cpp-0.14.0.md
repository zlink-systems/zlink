[한국어](./framework-cpp-0.14.0.ko.md) | [English](./framework-cpp-0.14.0.md)

# ZLink C++ Framework 0.14.0 release notes

Framework 0.14.0 uses binding 1.1.0 and Core 1.1.0. Framework releases are versioned independently per language.

## Highlights

- This release exists to ship the C++ packaging fixes in a published artifact. No user-visible API changes.
- Fixed a run-time SIGSEGV in programs installed through vcpkg: Core and the framework were compiling two different Boost trees into one binary.
- Fixed a packaging conflict where a shared vcpkg prefix caused every dependency's headers to be re-staged into this package.

## Installation

Download `zlink-framework-cpp-0.14.0.tar.gz` from the [`framework-cpp/v0.14.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.14.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and the Conan recipe also work.
