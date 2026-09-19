[English](./framework-cpp-0.18.2.md) | [한국어](./framework-cpp-0.18.2.ko.md)

# ZLink C++ Framework 0.18.2 Release Notes

Framework 0.18.2 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change.

## Common Changes

- Of the eight tutorial/samples zips attached to this GitHub Release, the .NET zips pin framework .NET 0.18.2 (binding 1.2.2, which works on Windows from the published packages alone). The C++ zips are identical to 0.18.1. For the 0.18.1 changes see the [0.18.1 release notes](./framework-cpp-0.18.1.md). (#702)

## Fixes

- When materializing a User Spot the runtime dropped the reservation's `AuthorityOwnerGeneration` on the way to the Spot context (defaulting to 1), so messages fenced with the correct generation never entered a turn. ZoneWorld's same-zone `UpdatePosition` on a relocation target timed out at ZW-B2 because of this. .NET passes the value through. (#665)
- The ZoneWorld sample now matches canonical §8: join completions are deduplicated by OperationId, and a same-zone move sends `UpdatePosition` to the Zone Spot to refresh the replica. This completes the seven samples' contract conformance in all four languages. (#665)

## Installation

Download `zlink-framework-cpp-0.18.2.tar.gz` from the [`framework-cpp/v0.18.2` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.2) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and Conan recipe are also available.
