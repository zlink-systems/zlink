[English](./framework-dotnet-0.15.0.md) | [한국어](./framework-dotnet-0.15.0.ko.md)

# ZLink .NET Framework 0.15.0 release notes

Framework 0.15.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- `IZLinkFanoutHandler<TEvent>.HandleAsync` now receives the publish context. The signature is `HandleAsync(TEvent message, ZLinkPublishMessageContext context, CancellationToken cancellationToken)`, so handler implementations take the added parameter. This matches the Java and Node surface and lets a handler read the topic the publisher chose.

## Changes

- The Classic fanout publisher takes a `SetNoDrop` setting. With it on, a record reaches every pipe whose topic matches or none of them, and a record that cannot be sent ends as `DeadlineExceeded`.
- A fanout subscriber registers the topics it receives with `Subscribe` at startup. It receives the byte-prefix union of the registered topics; with no registration it receives only the empty topic.
- Fanout publish waits for local admission before returning. Waiting past the send timeout ends as `DeadlineExceeded`, and the binding's send timeout now reaches Core's `SNDTIMEO`.
- Owner liveness is judged where a descriptor is first admitted, and a dead node's routing id is no longer taken as a peer.
- Omitting `SetAdvertiseHost` on a wildcard bind host advertises the loopback of the same address family: `127.0.0.1` for `0.0.0.0`, `::1` for `::`.
- A peer that finished admission with a confirmed RID can be named as a node-direct target.

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.15.0
```

The release tag is [`framework-dotnet/v0.15.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.15.0).
