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

The repository has no version-specific license inventory for the client and its resolved transitive dependencies. The proto license list does not replace that inventory. A license inventory tied to the lockfile and resolved graph remains outstanding.

[Client capability results](../../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.
