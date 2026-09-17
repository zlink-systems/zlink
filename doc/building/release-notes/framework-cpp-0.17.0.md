[English](./framework-cpp-0.17.0.md) | [한국어](./framework-cpp-0.17.0.ko.md)

# ZLink C++ Framework 0.17.0 release notes

Framework 0.17.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- Sending through a bound session from an Actor with no binding now ends the same way in all five languages. With no valid binding the call ends as `InvalidOperation`, and that failure surfaces at the **call's terminal** like every other call failure, rather than being thrown where the call is built.
- A push from an Actor with no binding used to complete while the frame was dropped; it now fails like the other four languages.

## Installation

Download `zlink-framework-cpp-0.17.0.tar.gz` from the [`framework-cpp/v0.17.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.17.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. A vcpkg overlay port and a Conan recipe are also available.
