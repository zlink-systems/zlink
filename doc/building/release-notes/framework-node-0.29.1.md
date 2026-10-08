[English](./framework-node-0.29.1.md) | [한국어](./framework-node-0.29.1.ko.md)

# ZLink Node.js Framework 0.29.1 Release Notes

Framework 0.29.1 uses Node.js binding 1.17.0 and Core 1.17.0.

## Changes

- The `StreamClient` tutorial pins `framework-codec-protobuf` to the Framework release version, and its README uses absolute URLs that resolve in exported mirrors. (#1562)

## Defect Fixes

- Fixed a 0.29.0 regression where an operation that arrived while the same Instance Spot was activating failed with `Unavailable` or `stale_target`. A later operation now joins the activation in progress and is processed in arrival order after Ready. (#1571)
- Fixed `Create` and `GetOrCreate` failing with `location owner lease is unavailable` after the owner node of an Actor was killed. For an Actor type whose factory registration disables relocation (`DisableRelocation`), the runtime releases the record whose owner lease has ended and creates a new incarnation. A type with relocation enabled still returns `Unavailable`. (#1570)
- Fixed Actor teardown starting dependency cleanup before already accepted handler turns completed. (#1579)

## Install

```bash
npm install @zlink-systems/framework@0.29.1 @zlink-systems/http-client@0.29.1
```

The release tag is [`framework-node/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.29.1).
[English](./framework-node-0.29.1.md) | [한국어](./framework-node-0.29.1.ko.md)
