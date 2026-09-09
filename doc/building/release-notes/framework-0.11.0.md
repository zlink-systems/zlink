[English](./framework-0.11.0.md) | [한국어](./framework-0.11.0.ko.md)

# ZLink Framework 0.11.0 Release Notes

## Dependency versions

Framework 0.11.0 uses ZLink binding 0.17.6 (Core 0.17.5) in C++, .NET, Java and Node.js.
0.10.0 used binding 0.17.3. Version numbers follow the [versioning policy](../versioning.md).

## Highlights

### Runtime

- .NET: when a ClientServer client's admission `hello` request times out (Core result 101), the
  runtime no longer records it as terminal; it restarts the next `hello` inside the same physical
  generation/attempt fence. On slow machines a single one-second timeout used to leave the
  connection intent permanently not-ready.
- Java and Node.js: after a ClientServer client's admission `hello` times out, the next `hello`
  restarts on the same physical connection (the same fix as .NET). The manual/local path used to treat the
  timeout as transport termination and the location path removed the connection.
- .NET: the remote relay multipart prefix expiry is now judged on the monotonic clock at append
  time too, so a late part no longer joins an expired prefix, and an earlier session request
  timeout no longer disposes the replacement correlation.
- Java: the public ClientServer weight is no longer passed to Core `peerWeight`. A server with
  weight 0 was classified Unavailable and selected instead of being excluded (NotFound).
- C++: the waitable timer `cancel(error_code&)` calls removed in Boost 1.87+ were replaced with the
  argument-less `cancel()`, which packaged Boost builds require.
- .NET and Node.js: the Location repository no longer treats a provider `Conflict` on the
  descriptor CAS as an immediate ownership loss (IgnoredStale). When the same host's owner-lease
  heartbeat landed between the CAS read and write, the `Preparing → Serving` publication failed
  permanently; the repository now re-reads the owner lease and the original predecessor after a
  conflict and, if both hold, re-issues the CAS with the refreshed version up to three times (the
  same rule as Java).

### Known issues

- .NET on macOS (arm64): in the unit test `DrainCoordinatorTests.Host_Stop_With_All_Channel_Kinds_…`
  the StreamNode session's Connected observation does not arrive within 5 seconds. Linux and Windows
  pass; the cause is investigated for 1.0.
- Node.js Windows CI: the Chromium Stream Connector E2E step does not finish (classified as a CI
  environment problem; Linux passes). Package behaviour is unaffected.

### Packages and builds

- .NET: the AssemblyName of `Zlink.Framework.AspNetCore` and `Zlink.HttpClient` is back to the
  `Zlink.*` package ID. The 0.10.0 packages carried `Systems.*` assembly names and an
  `InternalsVisibleTo`, so samples could not be built from the published packages alone.
- C++: the public asset used to contain only `framework/languages/cpp` and could not be installed.
  The source archive now includes the shared runtime inputs (`runtime/`: generated protocol
  headers, schema, golden and conformance files) and the LICENSE. Installed configs locate Core, the
  binding, Boost and LZ4 through `find_dependency` only and copy no files. The vcpkg overlay ports
  `zlink-cpp` and `zlink-framework` and the draft Conan recipes build this archive (checksums must
  be updated after the assets are published).
- C++: the repository build opens through CMake presets (`vs2022`, `windows-ninja`,
  `linux-ninja`, `macos-ninja`, `dev`, `ci`) and `scripts/dev/bootstrap-cpp.{sh,ps1}` (vcpkg plus
  overlay ports). Intel Mac is not supported.
- All languages: the seven samples and the per-language scenario e2e are out of the default
  build, CI and release. Inside the repository a sample references the framework source; copied
  outside it references the published packages automatically (two modes). See the
  [framework workspace layout](../framework-workspace.md).
- .NET and Java/Kotlin read the root `FRAMEWORK_VERSION` at build time. `sync-version.py` keeps
  the Node and C++ manifests aligned.

## Packages per language

- C++: `zlink-framework-cpp-0.11.0.tar.gz` and its SHA-256 file on the `framework/v0.11.0`
  GitHub Release
- Java/Kotlin: the framework modules in the `systems.zlink` namespace on Maven Central
- Node.js: the framework workspace packages in the `@zlink-systems` npm scope
- .NET: `Zlink.Framework`, `Zlink.Framework.AspNetCore`, `Zlink.HttpClient`,
  `Zlink.Stream.Connector` and the other packages on nuget.org

## Install examples

```bash
npm install @zlink-systems/framework@0.11.0
dotnet add package Zlink.Framework.AspNetCore --version 0.11.0
```

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.11.0")
}
```

For C++, download the GitHub Release asset, install the CMake package, and use
`find_package(zlink_framework_cpp CONFIG REQUIRED)`. Put the installed Core and binding package
prefixes on `CMAKE_PREFIX_PATH` as well.
