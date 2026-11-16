# Zlink.Framework.Locations.Etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../../doc/framework/dotnet/guide/server/25-location.en.md#store-setup) for installation and both registration calls.

## etcd client

Repository declarations and lock state as of 2026-11-15.

| Item | Current state | Evidence |
|---|---|---|
| Client | Uses stubs generated from the official etcd v3.6.16 protos with `Grpc.Net.Client`. | [Zlink.Framework.Locations.Etcd.csproj:7](Zlink.Framework.Locations.Etcd.csproj) |
| Version | Central declarations are `Grpc.Net.Client`, `Grpc.Core.Api` and `Grpc.Tools` 2.62.0, and `Google.Protobuf` 3.29.1. The provider directly references `Grpc.Net.Client`, `Google.Protobuf` and build-only `Grpc.Tools`. | [Directory.Packages.props:25](../../Directory.Packages.props) |
| Lockfile | No tracked `packages.lock.json` exists. Central version declarations and the package contract snapshot do not lock resolved transitive dependencies or content hashes. The provider also disables central transitive pinning. | [Zlink.Framework.Locations.Etcd.csproj:6](Zlink.Framework.Locations.Etcd.csproj) |

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md).

The repository has no version-specific license inventory for the client and its resolved transitive dependencies. The proto license list does not replace that inventory. A license inventory tied to the lockfile and resolved graph remains outstanding.

[Client capability results](../../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.
