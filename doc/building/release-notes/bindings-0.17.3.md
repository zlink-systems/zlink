[English](./bindings-0.17.3.md) | [한국어](./bindings-0.17.3.ko.md)

# ZLink Bindings 0.17.3 Release Notes (Draft)

> This is a draft. It is not a final release announcement until the registries and GitHub Releases are published.

## Highlights

- All four bindings embed or require Core 0.17.3. Core 0.17.3 fixes a receive-ownership race
  that could stall STREAM sockets during concurrent send/receive, insufficient entropy in the
  Windows random-byte source, and handling of a spurious mailbox wake during context termination.
- The Node.js binding now transfers unrouted multipart receives with two or more parts directly
  from N-API as `Buffer[]`. This reduces intermediate allocations while preserving the public
  `Received.parts` and ownership contracts.
- The C++ binding contract now matches the Core 0.17.2+ behavior that atomically admits an
  independent multipart record submitted by another thread.
- There are no intended public API changes in C++, Java, or .NET, and no C++ ABI change from
  0.17.2. The .NET NuGet artifact now carries MPL-2.0, repository/README/SourceLink metadata,
  and an snupkg symbol package.

Commits that only changed performance runners or documentation are excluded from this list.

## Installation

### Java

```kotlin
dependencies {
    implementation("systems.zlink:zlink:0.17.3")
}
```

The Maven Central group is `systems.zlink`.

### .NET

```bash
dotnet add package Systems.Zlink --version 0.17.3
```

Install `Systems.Zlink` from nuget.org.

### Node.js

```bash
npm install @zlink-systems/zlink@0.17.3
```

The first public release is published manually by the `zlink-systems` account. Subsequent releases
use npm Trusted Publishing with provenance.

### C++

- Use the `zlink-cpp-0.17.3.tar.gz` asset from the `cpp/v0.17.3` GitHub Release.
- Install the Core C library through the ConanCenter `zlink/0.17.3` recipe or the vcpkg `zlink`
  port. Keep both the C++ binding asset and Core package at 0.17.3.

## Compatibility

- Required Core release: `core/v0.17.3`
- Binding version: `0.17.3`
- Public API/ABI changes: none
