[한국어](./framework-node-0.25.0.ko.md) | [English](./framework-node-0.25.0.md)

# ZLink Node.js Framework 0.25.0 Release Notes

Framework 0.25.0 uses Node.js binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The GameQuest sample client expects the stale error for the first request routed to the retired owner after a one-way close, fixing the sample failure on Windows ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## Install

```bash
npm install @zlink-systems/framework@0.25.0 @zlink-systems/http-client@0.25.0
```

The release tag is [`framework-node/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.25.0).
