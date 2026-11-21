# zlink-framework-locations-etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../doc/framework/java/guide/server/25-location.en.md#store-setup) for installation and both registration calls. [Kotlin](../../../doc/framework/kotlin/guide/server/25-location.en.md#store-setup) uses the same Java packages.

## etcd client

Repository declarations and lock state as of 2026-11-20.

| Item | Current state | Evidence |
|---|---|---|
| Client | Uses Java gRPC stubs generated from the official etcd v3.6.16 protos. | [build.gradle.kts:58](build.gradle.kts) |
| Version | `grpc-netty-shaded`, `grpc-protobuf`, `grpc-stub` and `protoc-gen-grpc-java` are 1.70.0; `protobuf-java` and `protoc` are 4.30.2. Compile-only `javax.annotation-api` is 1.3.2 and the protobuf Gradle plugin is 0.9.4. | [build.gradle.kts:47](build.gradle.kts) |
| Lockfile | Resolvable configurations in this module use Gradle dependency locking. | [gradle.lockfile](gradle.lockfile) |

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../THIRD_PARTY_NOTICES.md).

The [dependency inventory](CLIENT_DEPENDENCIES.md) records resolved compile/runtime dependencies, code generators and licenses for shaded native components.

[Client capability results](../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.

## Client capability tests

`ZLinkEtcdTransportLimitTest` checks serialized authentication requests at 16 MiB−1, 16 MiB and 16 MiB+1, and responses at 8 MiB−1, 8 MiB and 8 MiB+1. Overflow produces an `IOException` based Store failure; oversized requests never reach the server. Response padding uses an unknown field to keep authentication tokens short.

Run Gradle commands from `framework/languages/java`.

```sh
flock -o ~/heavy.lock ./gradlew --no-daemon --max-workers=4 :zlink-framework-locations-etcd:test :zlink-framework-locations-etcd:contractTest
```

`ZLinkEtcdClientLiveTest` connects to a separate etcd instance with mTLS and password authentication. It writes and updates a unique key, then checks a revision-pinned Range: `keys_only=false` returns the original payload, while `true` omits only the value. It checks a successful secure connection and Store failures for a wrong password, untrusted CA, hostname mismatch and missing client certificate. It requires no provider rewrite or Store clock process. Host clock VM verification is outside this test's scope.

Set the following environment variables. Prepare an authenticated user with root privileges on a dedicated test etcd and require client certificates. The server certificate must contain DNS `localhost` and no IP SAN.

| Environment variable | Value |
|---|---|
| `ZLINK_ETCD_CLIENT_ENDPOINT` | `https://localhost:PORT` |
| `ZLINK_ETCD_CLIENT_USER`, `ZLINK_ETCD_CLIENT_PASSWORD` | Test username and password |
| `ZLINK_ETCD_CLIENT_CA` | CA PEM file path |
| `ZLINK_ETCD_CLIENT_CERT`, `ZLINK_ETCD_CLIENT_KEY` | Client certificate and private key PEM file paths |

```sh
flock -o ~/heavy.lock ./gradlew --no-daemon --max-workers=4 :zlink-framework-locations-etcd:integrationTest --tests '*ZLinkEtcdClientLiveTest'
```

Live tests are skipped when environment variables are absent. Record fake-server `ZLinkEtcdTlsTest` results separately from real etcd executions.
