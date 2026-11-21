# framework-locations-etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../../doc/framework/cpp/guide/server/25-location.en.md#store-setup) for installation and both registration calls.

## etcd client

Repository declarations and lock state as of 2026-11-15.

| Item | Current state | Evidence |
|---|---|---|
| Client | Uses C++ gRPC stubs generated from the official etcd v3.6.16 protos. | [CMakeLists.txt:3](CMakeLists.txt) |
| Version | Conan declares `grpc/1.84.0` with `with_etcd`; the shared declarations are `protobuf/6.33.5` and `abseil/20260107.1`. | [conanfile.py:73](../../packaging/conan/conanfile.py), [conanfile.py:27](../../packaging/conan/conanfile.py) |
| Lockfile | The four Conan locks are the prebuilt package graph (`with_etcd=False`): they pin protobuf 6.33.5 and abseil 20260107.1 and contain no gRPC entry. The exact version in conanfile.py fixes the etcd gRPC version. | [conan.linux-x64.lock:3](../../packaging/conan/conan.linux-x64.lock) |
| vcpkg | `builtin-baseline` is pinned to the CI vcpkg commit `2ec0f1747d`; overrides fix grpc 1.84.0, abseil 20260107.1 and protobuf 6.33.4 (port-version 2). The vcpkg registry has no protobuf 6.33.5, so it differs from Conan by one patch release. | [vcpkg.json:4](../../vcpkg.json), [vcpkg.json:35](../../vcpkg.json) |
| CMake | `find_package(gRPC CONFIG REQUIRED)` does not constrain the version. The Conan declarations and vcpkg overrides above fix it. | [CMakeLists.txt:671](../../CMakeLists.txt) |

[linux-x64:3](../../packaging/conan/conan.linux-x64.lock) · [linux-arm64:3](../../packaging/conan/conan.linux-arm64.lock) · [macos-arm64:3](../../packaging/conan/conan.macos-arm64.lock) · [windows-x64:3](../../packaging/conan/conan.windows-x64.lock)

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md).

The etcd-enabled Linux release test graph is recorded in the
[resolved inventory](dependencies/vcpkg-resolved.json). It connects package versions,
port versions, features, ABIs and dependency edges to SHA-256 hashes of each `CONTROL` and full
vcpkg [license notice](dependencies/licenses/). Only trailing whitespace and blank lines
at EOF are removed; `sourceNoticeSha256` records the original bytes. This inventory is not a vcpkg lockfile
and does not cover other triplets or the Conan resolved graph. The gRPC notice includes
BSD-3-Clause and MPL-2.0 file sections alongside Apache-2.0; retain the full notice.
The compiler and libc are outside this gRPC package closure.

[Client capability results](../../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.

## Client capability tests

The following tests in `test_cpp_framework_locations_etcd` use the real gRPC transport.

- `MaximumSendLimitUsesStoreFailureBeforeServerDispatch` checks that
  AuthenticateRequest `ByteSizeLong()` is 16 MiB−1/16 MiB/16 MiB+1. The first two
  cases each reach Authenticate and Txn once; the oversized case reaches neither
  service and returns Store failure.
- `ExactSerializedReceiveLimitUsesStoreFailure` checks the complete AuthenticateResponse,
  including token and unknown field, at 8 MiB−1/8 MiB/8 MiB+1. The oversized response
  reaches Authenticate once, never reaches Txn, and returns Store failure.
- `NoResponseDeadlineMovesNextOperation` observes server `IsCancelled()` after the
  operation deadline. `ConsumerWaitCancellationDoesNotCancelOrRetirePhysicalOperation`
  checks that consumer wait cancellation leaves the physical RPC active and retained
  by the provider until completion.
- `MutualTlsCustomCaAndIdentityHostNameAndTrustValidation` and authentication refresh
  tests exercise acceptance and rejection against a fake gRPC server.

`EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls` uses an actual secured etcd server.
Start a dedicated etcd v3.6.16 instance with `--client-cert-auth`, CA, server certificate
and key; create root, grant its root role, and run `etcdctl auth enable`.
Test certificates are in `tests/tls/`; the server certificate hostname is `localhost`.
The test creates and removes two fixed keys in namespace `cpp-capability`, so use a
separate test instance.

```sh
export ZLINK_ETCD_SECURE_ENDPOINT=https://localhost:2379
export ZLINK_ETCD_ROOT_PASSWORD=your-test-root-password
<build-dir>/test_cpp_framework_locations_etcd \
  --gtest_filter=EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls
```

The test writes a nonempty payload, pins its revision, changes the value, then reads
with `keys_only=false/true`. Key, create/mod revision and version must match; values
must be the original bytes and empty bytes respectively. The public provider must
also read successfully with authentication and mTLS, and return Store failure for
an incorrect password, CA, hostname or missing client identity. Missing environment
variables skip the test and do not count as security verification. Physical cancellation
coverage is limited to operation deadlines; no public cancellation option is added.

On 2026-11-20, Linux x86_64 tests used gRPC 1.84.0 and the resolved inventory above.
`--gtest_filter=-EtcdLive.*` passed all 34 tests, and
`EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls` passed against actual etcd 3.6.16
with authentication and mTLS enabled. The latter includes rejection of an incorrect
password, CA, hostname and missing client identity.
