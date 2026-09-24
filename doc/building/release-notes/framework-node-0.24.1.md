[한국어](./framework-node-0.24.1.ko.md) | [English](./framework-node-0.24.1.md)

# ZLink Node.js Framework 0.24.1 Release Notes

Framework 0.24.1 uses Node.js binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The Node.js Framework contract and behavior are unchanged.

## Install

```bash
npm install @zlink-systems/framework@0.24.1 @zlink-systems/http-client@0.24.1
```

The release tag is [`framework-node/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.24.1).
