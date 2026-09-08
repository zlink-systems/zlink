[English](./framework-0.10.0.md) | [한국어](./framework-0.10.0.ko.md)

# ZLink Framework 0.10.0 Release Notes (Draft)

> This is a draft. Publish Framework only after the public 0.17.3 binding release is complete.

## Dependency Version

Framework 0.10.0 consumes ZLink binding 0.17.3 in C++, .NET, Java, and Node.js. The Framework
version remains 0.10.0.

## Packages by Language

- C++: `zlink-framework-cpp-0.10.0.tar.gz` and its SHA-256 file from the
  `framework/v0.10.0` GitHub Release
- Java/Kotlin: Framework modules under the Maven Central `systems.zlink` namespace
- Node.js: Framework workspace packages under the npm `@zlink-systems` scope
- .NET: `Zlink.Framework.AspNetCore`, `Zlink.HttpClient`, and
  `Zlink.Stream.Connector` on nuget.org

The former .NET package IDs `Zlink.Framework.AspNetCore` and `Zlink.HttpClient` become
`Zlink.Framework.AspNetCore` and `Zlink.HttpClient`, respectively. Source namespaces
remain under `Zlink.*` for compatibility.

## Installation Examples

```bash
npm install @zlink-systems/framework@0.10.0
dotnet add package Zlink.Framework.AspNetCore --version 0.10.0
```

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.10.0")
}
```

For C++, download and install the GitHub Release asset, then use
`find_package(zlink_framework_cpp CONFIG REQUIRED)`.

## Release Order

1. Publish the C++, Node.js, Java, and .NET bindings at 0.17.3.
2. Verify 0.17.3 in the public registries and GitHub Releases.
3. Publish Framework 0.10.0. Push the .NET packages from `release-dotnet.yml`, the workflow filename
   bound to the nuget.org Trusted Publishing policy.
