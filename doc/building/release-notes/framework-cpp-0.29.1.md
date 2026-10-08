[English](./framework-cpp-0.29.1.md) | [한국어](./framework-cpp-0.29.1.ko.md)

# ZLink C++ Framework 0.29.1 Release Notes

Framework 0.29.1 uses C++ binding 1.17.0 and Core 1.17.0.

## Defect Fixes

- Fixed a 0.29.0 regression where an operation that arrived while the same Instance Spot was activating failed with `Unavailable` or `stale_target`. A later operation now joins the activation in progress and is processed in arrival order after Ready. (#1571)
- Fixed `Create` and `GetOrCreate` failing with `location owner lease is unavailable` after the owner node of an Actor was killed. For an Actor type whose factory registration disables relocation (`DisableRelocation`), the runtime releases the record whose owner lease has ended and creates a new incarnation. A type with relocation enabled still returns `Unavailable`. (#1570)

## Install

Select a platform Framework archive from the [`framework-cpp/v0.29.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.1), or download it with `bootstrap.cmake`. A server Framework requires C++ binding 1.17.0 and Core 1.17.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.1).
[English](./framework-cpp-0.29.1.md) | [한국어](./framework-cpp-0.29.1.ko.md)
