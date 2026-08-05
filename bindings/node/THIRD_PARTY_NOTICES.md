# Third-Party Notices

This repository contains third-party source code and/or redistributable binaries.
This file summarizes known components and where their license texts are located.

## Bundled source dependencies

- Boost (header-only subset)
  - Location: `core/external/boost/`
  - License text: `core/external/boost/LICENSE_1_0.txt`
  - License type: Boost Software License 1.0

- moodycamel::ConcurrentQueue
  - Location: `core/external/moodycamel/`
  - License text: `core/external/moodycamel/LICENSE`
  - License type: Simplified BSD (2-Clause)

- wepoll
  - Location: `core/external/wepoll/`
  - License text: `core/external/wepoll/license.txt`
  - License type: BSD-2-Clause style

- Unity test framework
  - Location: `core/external/unity/`
  - License text: `core/external/unity/license.txt`
  - License type: MIT

- SHA1 implementation (WIDE Project)
  - Location: `core/external/sha1/`
  - License text: `core/external/sha1/license.txt`
  - License type: BSD-3-Clause style

## Bundled runtime binaries in bindings

The following folders include prebuilt runtime binaries for convenience:

- `bindings/node/prebuilds/`
- `bindings/dotnet/runtimes/*/native/`
- `bindings/java/src/main/resources/native/`
- `bindings/cpp/native/`
- `bindings/python/src/zlink/native/`

These artifacts may include components such as:

- zlink/libzlink binaries
- Microsoft Visual C++ runtime DLLs (Windows targets)
- OpenSSL runtime libraries, depending on package/target

When redistributing these artifacts, ensure the corresponding third-party
license obligations are met for your target platform and package format.

## Origin of `core/`

`core/` began as a fork of [libzmq](https://github.com/zeromq/libzmq) v4.3.5,
narrowed to a smaller set of socket patterns and re-platformed onto Boost.Asio
and OpenSSL (see `core/doc/guide/design-rationale.ko.md` for the technical
rationale). Separately, as public upstream history: libzmq v4.3.5 is the
release at which the libzmq project itself completed relicensing from
LGPLv3 + static-linking exception to MPL-2.0. `core/` therefore carries the
MPL-2.0 license forward from that upstream lineage. The upstream copyright
holders and contributors are recorded verbatim (as of v4.3.5) in
[`core/AUTHORS`](./core/AUTHORS), vendored from libzmq for that reason.

## Project License

This repository uses three licenses, split by layer:

- `core/` and `bindings/` (per-language native bindings) are licensed under
  MPL-2.0. See `LICENSE`.
- `framework/` (the higher-level framework built on top of the bindings) is
  licensed under FSL-1.1-ALv2 (Functional Source License 1.1, Apache-2.0
  Future License). See `framework/LICENSE`.
- Each language's `http-client` package under `framework/` (a thin wrapper
  over that platform's commonly used HTTP client library — .NET's
  `System.Net.Http`, Java/Kotlin's `java.net.http`, Node's `undici`, C++'s
  Boost.Beast) is licensed under Apache License 2.0 instead of FSL, because
  it wraps a permissively-licensed library rather than any zlink-original
  transport. The .NET and Node packages declare this directly in their own
  manifest; the Java and Kotlin packages receive it from the shared Gradle
  publish configuration; the C++ package ships the license text alongside
  its sources.

Unless otherwise noted, source code outside `framework/` is licensed under
MPL-2.0.
