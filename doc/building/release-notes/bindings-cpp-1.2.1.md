[English](./bindings-cpp-1.2.1.md) | [한국어](./bindings-cpp-1.2.1.ko.md)

# ZLink C++ binding 1.2.1 release notes

This release runs on Core 1.2.0; Core is unchanged.

## Changes

- The ROUTER receive path now forwards Core 1.2.0's typed no-data result from `zlink_recv` instead of converting it to `-1`. Callers can observe the result, and Framework C++ receive-timeout handling preserves the Core result (#273).
## Verification

- The Core version remains 1.2.0.

The release tag is `cpp/v1.2.1`.
