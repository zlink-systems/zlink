# framework-locations-etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../../doc/framework/cpp/guide/server/25-location.en.md#store-setup) for installation and both registration calls.

## etcd client

Repository declarations and lock state as of 2026-11-15.

| Item | Current state | Evidence |
|---|---|---|
| Client | Uses C++ gRPC stubs generated from the official etcd v3.6.16 protos. | [CMakeLists.txt:3](CMakeLists.txt) |
| Version | Conan declares `grpc/1.69.0` with `with_etcd`; the shared protobuf declaration is `protobuf/5.27.0`. | [conanfile.py:70](../../packaging/conan/conanfile.py) |
| Lockfile | The resolved etcd client graph is not pinned. The four Conan locks below contain protobuf entries but no gRPC entry. | [conan.linux-x64.lock:3](../../packaging/conan/conan.linux-x64.lock) |
| vcpkg | The `etcd` feature requires `grpc`, with no `builtin-baseline` or version override. | [vcpkg.json:21](../../vcpkg.json) |
| CMake | `find_package(gRPC CONFIG REQUIRED)` does not constrain the version. CI vcpkg results do not establish results for the declared Conan version. | [CMakeLists.txt:671](../../CMakeLists.txt) |

[linux-x64:3](../../packaging/conan/conan.linux-x64.lock) · [linux-arm64:3](../../packaging/conan/conan.linux-arm64.lock) · [macos-arm64:3](../../packaging/conan/conan.macos-arm64.lock) · [windows-x64:3](../../packaging/conan/conan.windows-x64.lock)

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md).

The repository has no version-specific license inventory for the client and its resolved transitive dependencies. The proto license list does not replace that inventory. A license inventory tied to the lockfile and resolved graph remains outstanding.

[Client capability results](../../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.
