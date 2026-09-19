[English](./bindings-node-1.2.1.md) | [한국어](./bindings-node-1.2.1.ko.md)

# ZLink Node.js binding 1.2.1 release notes

This release runs on Core 1.2.0. Core is unchanged; only the packaging is fixed.

## Changes

- The npm package now ships `prebuilds/win32-x64` alongside `linux-x64`. 1.2.0 carried only `linux-x64`, so `npm install` on Windows fell back to a node-gyp build and demanded `ZLINK_CORE_INSTALL_PREFIX`. Windows x64 now installs without a native build. (#656)
- A source build proceeds with a warning when the Core prefix has no `core-package-provenance.json`, which is the case for a public Core archive extracted by hand. The header and library existence checks remain. (#656)

## Verification

- In an empty directory outside the repository, with no `ZLINK_*` variables, `npm install` of the tarball ran no node-gyp and `require('@zlink-systems/zlink')` worked.

The release tag is `node/v1.2.1`.
