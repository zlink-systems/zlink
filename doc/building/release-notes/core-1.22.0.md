[English](./core-1.22.0.md) | [한국어](./core-1.22.0.ko.md)

# libzlink 1.22.0 release notes

## Changes

- Excluded internal socket progress from public API admission. Fixed `EBUSY` on socket close immediately after monitor detachment, which could leave `zlink_ctx_term` waiting indefinitely (#3444, #3448).
- Replaced idle socket monitor polling every 10 ms with explicit delivery scheduling when an event is enqueued (#2277, #3245).

## Compatibility

There are no public API or ABI changes; linker SOVERSION remains 0. All seven binding versions and the exact Core/binding pins of all four Framework languages move to 1.22.0. Framework versions remain 0.31.0. Component compatibility follows the [version policy](../versioning.md).

## Binding changes

There are no binding code changes in `core/v1.21.0..origin/main`. The only binding change in this range updates the C++ Conan recipe checksum for the 1.21.0 release asset (#3220). The vcpkg and Conan checksums for 1.22.0 assets will be updated in a separate PR after the tag and assets exist.
