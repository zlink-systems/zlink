# [zlink] Add version 0.17.3

## Description

This adds the first ConanCenter recipe for zlink 0.17.3, a high-performance
asynchronous messaging library. The recipe builds either the shared or static
library, optionally enables TLS through OpenSSL, and exposes the upstream CMake
targets `libzlink` and `libzlink-static`.

The source is the dedicated `zlink-0.17.3-source.tar.gz` asset attached to the
immutable `core/v0.17.3` GitHub release.

## Checklist

- [x] Added only the latest release to `config.yml` and `conandata.yml`.
- [x] Pinned the release source with its SHA-256 digest.
- [x] Declared license, homepage, topics, package type, options, and OpenSSL dependency.
- [x] Added C++17 validation and Conan CMake layout/toolchain/dependency metadata.
- [x] Added an explicit `test_package` that configures, links, and runs.
- [x] Tested both shared and static package variants locally.
- [ ] Signed the ConanCenter CLA with the submitting GitHub account.
- [ ] Ran ConanCenter CI in the submitted pull request.

## Tested platform

- Linux 6.6 (WSL2), x86_64
- Ubuntu 24.04 toolchain, GCC 13.3.0
- Conan 2.32.0
- `conan create core/packaging/conan --version 0.17.3 -s build_type=Release`
- `conan create core/packaging/conan --version 0.17.3 -s build_type=Release -o 'zlink/*:shared=False'`

No pull request has been submitted from this draft.
