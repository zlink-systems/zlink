[English](./framework-node-0.30.2.md) | [한국어](./framework-node-0.30.2.ko.md)

# ZLink Node.js Framework 0.30.2 Release Notes

Framework 0.30.2 uses Node.js binding 1.20.0 and Core 1.20.0.

## Compatibility changes

- `addEntrySpot` takes the Actor stable type that the Entry Spot accepts, together with the Entry Spot type. TypeScript Actor type arguments do not exist at run time, so without this value a new Actor could be created on a node that does not accept its type. Pass the same name that `addActorFactory` uses. (#1772)

  ```ts
  // 0.30.1
  objects.addEntrySpot(LobbySpot);
  // 0.30.2
  objects.addEntrySpot(LobbySpot, 'player');
  ```

## Changes

- Records the request source owner at ingress and preserves the original requester after relocation. Actor and Spot requests are handled only when the source descriptor is valid. (#2103, #2140, #2151)
- Reduced per-message Store reads and completion work, and aligned runtime metric labels and refresh points with the contract. (#1809, #1834, #1866, #2189, #2190)
- Deferred Join and HTTP handler Yield now continue on the correct execution turn, including consecutive deferral. (#2003, #2064)
- Transfers pending timers and frames to the new owner during Actor relocation and updates relocation outcome metrics. (#1987, #1960, #2013)

## Defect Fixes

- Fixed stalls when Spot close and Session seal could not wait for a running handler or the close of an open binding. (#1896, #1915, #2135, #1886)
- Fixed mismatched lifetimes and authority during deferred Join, source settlement, and relocation completion. (#1903, #1938, #2050, #2003)
- Requests without a source descriptor are rejected, and the original requester is preserved during relocation to prevent misrouted results. (#2103, #2140, #2151)
- Location capacity metrics now refresh with descriptor polling, and outcome metrics use only the defined labels. (#2189, #2190)
- Updated the Bingo sample so an observer looks up the room's current state and confirmed rewards after joining. (#2427)
- The original exception from target Join lifecycle failure is reported through the error reporter. (#2459)
- Core 1.20.0 no longer stalls the temporary transport owner's executor when a completion poller is registered during the handshake. (#2504)
- Actor replies are sent as a single STREAM frame, and error payloads use the spec's `{code, message}` form. All four languages verify against the same shared vectors. (#2507)
- An Actor Response is validated against the request packet's reply contract. It was validated against the payload contract, so a typed response to an Entry Spot Actor request could be rejected. (#2507)
- A request rejected by admission during shutdown now returns a `ShuttingDown` error to the requester. Work waiting on that request kept shutdown open until a forced stop after 30 seconds. A rejected one-way send is dropped without a reply. (#2620)
- Fixed the `Continued` callback running after a held application job when a User Spot retire is aborted. (#2623)

## Install

```bash
npm install @zlink-systems/framework@0.30.2 @zlink-systems/http-client@0.30.2
```

The release tag is [`framework-node/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.30.2).
[한국어](./framework-node-0.30.2.ko.md) | [English](./framework-node-0.30.2.md)
