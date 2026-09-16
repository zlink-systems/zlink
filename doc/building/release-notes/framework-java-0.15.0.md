[English](./framework-java-0.15.0.md) | [한국어](./framework-java-0.15.0.ko.md)

# ZLink Java Framework 0.15.0 release notes

Framework 0.15.0 uses binding 1.2.1 and Core 1.2.0. Each framework language is versioned independently. The Kotlin artifacts ship with this release.

## Changes

- The Classic fanout publisher takes a `setNoDrop` setting. With it on, a record reaches every pipe whose topic matches or none of them, and a record that cannot be sent ends as `DeadlineExceeded`.
- A fanout subscriber registers the topics it receives with `subscribe` at startup. It receives the byte-prefix union of the registered topics; with no registration it receives only the empty topic.
- Fanout publish waits for local admission before returning. Waiting past the send timeout ends as `DeadlineExceeded`.
- Owner liveness is judged where a descriptor is first admitted. Manual peers and relocation select only live descriptors.
- Omitting `setAdvertiseHost` on a wildcard bind host advertises the loopback of the same address family: `127.0.0.1` for `0.0.0.0`, `::1` for `::`.
- The first parameter of `ZLinkRouteClient.sendToNode` and `requestToNode` is named `meshName`.
- This release uses binding 1.2.1. The 1.2.0 jar carried no Windows native, so it could not run on Windows without `ZLINK_LIBRARY_PATH`.

## Installation

```kotlin
implementation("systems.zlink:zlink-framework-core:0.15.0")
```

The release tag is [`framework-java/v0.15.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.15.0).
