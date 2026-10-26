[English](./framework-cpp-0.30.3.md) | [한국어](./framework-cpp-0.30.3.ko.md)

# ZLink C++ Framework 0.30.3 Release Notes

Framework 0.30.3 uses C++ binding 1.20.0 and Core 1.20.0.

## Defect Fixes

- Fixed `GetOrCreate` failing to Join a User Spot while that Spot was closing. (#2672)
- Fixed an exception when calling `Close` with a stale `SpotRef` after the Spot had closed. A second `Close` call joins an in-progress `Close` and receives the same result. (#2675)

## Install

Select a platform Framework archive from the [`framework-cpp/v0.30.3` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.3), or download it with `bootstrap.cmake`. A server Framework requires C++ binding 1.20.0 and Core 1.20.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.30.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.3).
