# zlink-framework-locations-etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../doc/framework/java/guide/server/25-location.en.md#store-setup) for installation and both registration calls. [Kotlin](../../../doc/framework/kotlin/guide/server/25-location.en.md#store-setup) uses the same Java packages.

## etcd client

Repository declarations and lock state as of 2026-11-15.

| Item | Current state | Evidence |
|---|---|---|
| Client | Uses Java gRPC stubs generated from the official etcd v3.6.16 protos. | [build.gradle.kts:58](build.gradle.kts) |
| Version | `grpc-netty-shaded`, `grpc-protobuf`, `grpc-stub` and `protoc-gen-grpc-java` are 1.70.0; `protobuf-java` and `protoc` are 4.30.2. Compile-only `javax.annotation-api` is 1.3.2 and the protobuf Gradle plugin is 0.9.4. | [build.gradle.kts:47](build.gradle.kts) |
| Lockfile | No tracked Gradle dependency lockfile exists. Direct dependency version declarations do not lock resolved transitive dependencies. | [build.gradle.kts:47](build.gradle.kts) |

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../THIRD_PARTY_NOTICES.md).

The repository has no version-specific license inventory for the client and its resolved transitive dependencies. The proto license list does not replace that inventory. A license inventory tied to the lockfile and resolved graph remains outstanding.

[Client capability results](../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.
