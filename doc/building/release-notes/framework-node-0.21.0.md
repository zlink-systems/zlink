[English](./framework-node-0.21.0.md) | [한국어](./framework-node-0.21.0.ko.md)

# ZLink Node.js Framework 0.21.0 Release Notes

Framework 0.21.0 uses binding 1.2.1 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- None. The public contract is the same as 0.20.0.

## Common Changes

- The Unity WebGL bundle (`zlink-stream-connector.jspre`) is resynchronized with the stream-connector build output and the gate checks for a tracked diff after a build. (#826)
- The runtime and the public contract are unchanged from 0.20.0. This release cleans up the tutorial, samples, quickstart and the repository tooling.
- Quickstart, tutorial and samples are obtained from the per-language examples repository (`zlink-<lang>-examples`); the mirror workflow preserves executable bits (#831) and the README starts with an English | 한국어 switch.
- The tutorial CI C++ job keeps the vcpkg binary cache in the Actions cache. (#852)
- google-java-format 1.27.0 runs the Java and Kotlin format check under JDK 25. (#798)
- The unused v11 public-contract trace generator and inventories are removed. (#747)

## Install

```bash
npm install @zlink-systems/framework@0.21.0 @zlink-systems/http-client@0.21.0
```

The release tag is [`framework-node/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.21.0).
