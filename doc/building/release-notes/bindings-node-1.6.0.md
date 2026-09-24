[English](./bindings-node-1.6.0.md) | [한국어](./bindings-node-1.6.0.ko.md)

# ZLink Node.js binding 1.6.0 release notes

This release aligns the Node.js binding package with Core 1.6.0.

## Changes

- The npm package version is 1.6.0 for the Core 1.6.0 release.
- Core 1.6.0 allows ROUTER to publish a reply token after a complete REQUEST is received, even if the source pipe disconnects. The public C API and ABI are unchanged ([#1051](https://github.com/zlink-systems/zlink/issues/1051)).
- This release changes no Node.js binding behavior; it aligns the binding version with Core 1.6.0.

## Verification

- The release preflight verifies the Node.js binding version, release notes, and npm package metadata.

The release tag is `node/v1.6.0`.
