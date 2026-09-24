[English](./bindings-node-1.7.0.md) | [한국어](./bindings-node-1.7.0.ko.md)

# ZLink Node.js binding 1.7.0 release notes

This release aligns the Node.js binding package with Core 1.7.0.

## Changes

- The npm package version is 1.7.0 and uses Core 1.7.0.
- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- This release changes no Node.js binding behavior.

## Verification

- The release preflight verifies the Node.js binding version, release notes, and npm package metadata.

The release tag is `node/v1.7.0`.
