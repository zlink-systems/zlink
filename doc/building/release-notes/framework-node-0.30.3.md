[English](./framework-node-0.30.3.md) | [한국어](./framework-node-0.30.3.ko.md)

# ZLink Node.js Framework 0.30.3 Release Notes

Framework 0.30.3 uses Node.js binding 1.20.0 and Core 1.20.0.

## Defect Fixes

- Fixed `GetOrCreate` failing to Join a User Spot while that Spot was closing. (#2672)
- Fixed an exception when calling `Close` with a stale `SpotRef` after the Spot had closed. A second `Close` call joins an in-progress `Close` and receives the same result. (#2675)
<!-- #2673 -->

## Install

```bash
npm install @zlink-systems/framework@0.30.3 @zlink-systems/http-client@0.30.3
```

The release tag is [`framework-node/v0.30.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.30.3).
