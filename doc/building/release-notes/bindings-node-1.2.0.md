[English](./bindings-node-1.2.0.md) | [한국어](./bindings-node-1.2.0.ko.md)

# ZLink Node.js binding 1.2.0 release notes

This release runs on Core 1.2.0. The 1.1.0 binding assumes the Core 1.1.0 ABI and cannot be used with Core 1.2.0; this release closes that gap.

## Changes

- Binds to the Core 1.2.0 native library. The native library and provenance in the package are the Core 1.2.0 release assets.
- Emits gyp-safe Core paths so node-gyp configure succeeds on Windows.
## Verification

- The binding test suite passes on the Core 1.2.0 package.

The release tag is `node/v1.2.0`.
