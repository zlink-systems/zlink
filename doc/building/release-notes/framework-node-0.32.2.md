[English](./framework-node-0.32.2.md) | [한국어](./framework-node-0.32.2.ko.md)

# ZLink Node.js Framework 0.32.2 Release Notes

Framework 0.32.2 is a hotfix for defects in 0.32.1. It keeps Node.js binding 1.23.0 and Core 1.23.0.

## Fixes

- A request sent to an Actor at the moment it moved from a User Spot to the Entry Spot (for example with `joinEntrySpot().defer()`) could end with `SPOT actor is not registered locally` (ActorLocationStale). A message that entered the previous Spot's Actor queue before the move was judged only by the "left this Spot" mark after the move completed, so the Actor was treated as missing. The message now moves to the Actor's current Spot queue with its original correlation, deadline and reply route, and runs once. The opposite direction (Entry → User Spot) already succeeded and is unchanged. (#4437, #4455)

## Upgrading from 0.30.x

- `ZLinkMessageContext.receivedAt` is required since 0.32.0. Code that implements `ZLinkMessageContext` itself, such as fake contexts in tests, must fill `receivedAt` (the UTC time the Framework received the message). (#3602)

## Install

```bash
npm install @zlink-systems/framework@0.32.2
```

The release tag is [`framework-node/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.2).
