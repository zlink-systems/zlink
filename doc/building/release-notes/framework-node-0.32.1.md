[English](./framework-node-0.32.1.md) | [한국어](./framework-node-0.32.1.ko.md)

# ZLink Node.js Framework 0.32.1 Release Notes

Framework 0.32.1 is a hotfix for defects in 0.32.0. It keeps Node.js binding 1.23.0 and Core 1.23.0.

## Bug fixes

- When the Location Store failed once to delete an Actor authority, the Store kept pointing at an Actor the node no longer had: `getOrCreate` returned that incarnation and destroying the same ref ended with a generation mismatch, so there was no way out. Destroy now keeps the exact incarnation until cleanup finishes and returns the Store delete failure as a destroy failure. Destroying the same ref again retries the Store cleanup. (#4397, #4403)

## Upgrading from 0.30.x

- `ZLinkMessageContext.receivedAt` is required since 0.32.0. Code that implements `ZLinkMessageContext` itself, such as a fake context in tests, must set `receivedAt` (the UTC time the Framework accepted the message). (#3602)

## Install

```bash
npm install @zlink-systems/framework@0.32.1
```

The release tag is [`framework-node/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.1).
