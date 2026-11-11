# Store Plugin Choices

Choose one of these two store plugins. Register the two Stores separately.

| Plugin | Location Store | Relocation Store | Use when |
| --- | --- | --- | --- |
| Redis | Redis | Redis (same server) | Deployments meeting the [Redis durability condition](../../../common/spec/server/05-location-relocation/02-location-store-redis.en.md#1-scope-and-audience). |
| Redis with etcd | etcd (3 voters) | Redis | Deployments needing automatic failover. |

The Redis durability requirements are defined by [Location Store Redis](../../../common/spec/server/05-location-relocation/02-location-store-redis.en.md); etcd deployment and recovery requirements are defined by [Location Store etcd](../../../common/spec/server/05-location-relocation/07-location-store-etcd.en.md).

Installation and registration by language:

- [C#/.NET](../../../dotnet/guide/server/25-location.en.md#store-setup)
- [C++](../../../cpp/guide/server/25-location.en.md#store-setup)
- [Java](../../../java/guide/server/25-location.en.md#store-setup)
- [Kotlin](../../../kotlin/guide/server/25-location.en.md#store-setup)
- [Node/TypeScript](../../../node/guide/server/25-location.en.md#store-setup)
