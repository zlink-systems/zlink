[English](./framework-node-0.15.0.md) | [한국어](./framework-node-0.15.0.ko.md)

# ZLink Node.js Framework 0.15.0 release notes

Framework 0.15.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Changes

- The Classic fanout publisher takes a `setNoDrop` setting. With it on, a record reaches every pipe whose topic matches or none of them, and a record that cannot be sent ends as `DeadlineExceeded`.
- A fanout subscriber registers the topics it receives with `subscribe` at startup. It receives the byte-prefix union of the registered topics; with no registration it receives only the empty topic.
- Fanout publish waits for local admission before returning. Waiting past the send timeout ends as `DeadlineExceeded`.
- Owner liveness is judged where a descriptor is first admitted.
- Omitting `setAdvertiseHost` on a wildcard bind host advertises the loopback of the same address family: `127.0.0.1` for `0.0.0.0`, `::1` for `::`. Startup used to be rejected instead.
- Receiving happens in both dispatch modes, so a Manual wait needs no pump of its own.

## Installation

```bash
npm install @zlink-systems/framework@0.15.0
```

The release tag is [`framework-node/v0.15.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.15.0).
