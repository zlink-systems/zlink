[English](./framework-bindings-dependency-boundary.md) | [한국어](./framework-bindings-dependency-boundary.ko.md)

# The Framework–Bindings Dependency Boundary

This document sets out how the framework language implementations should reference the bindings
libraries. The goal is to keep the framework from being tied directly to the bindings source tree
while keeping local development and CI verification practical.

This document does not guarantee the current public distribution state. Whether the packages are
actually published to Maven, Conan, NuGet, or npm has to be confirmed separately. The repository
carries the configuration and workflows for publishing, but the framework build must not be
designed on the assumption that every registry always has a current release.

## 1. The problem

Referencing bindings source directly from the framework creates these problems.

- While bindings code is being modified, framework build and test results move with it.
- The code a local framework references can diverge from the package a real user installs.
- Framework tests cannot catch a missing native runtime, type declaration, CMake config, or piece
  of metadata that the package should have carried.
- It becomes easy for the framework to lean on the bindings' internal structure. The framework must
  use only the bindings' public API.

So the default build path must reference the package or install artifact the bindings produce, not
the bindings source. A bindings source reference stays as an exception path, switched on explicitly
and only when debugging the bindings themselves.

## 2. Common principles

### 2.1 The default is a package reference

The framework's default build references the bindings through the language's package manager or
install path.

| Language | Default reference | Local verification |
|------|----------------|----------------|
| .NET | NuGet package | repo-local NuGet feed |
| Java/Kotlin | Maven artifact | repo-local Maven repository or `mavenLocal()` |
| Node.js | npm package | a tarball from `npm pack`, or a local registry |
| C++ | CMake package | install prefix, vcpkg overlay, Conan local cache |

Copying DLL, JAR, `.node`, or `.so` files into the framework and pinning them there is not
recommended. Copying looks simple at first, but it pushes per-platform artifacts, versions, runtime
assets, and stale-artifact checks outside the build system. It leaves a person managing by hand
what a package manager or CMake package should be handling.

### 2.2 Source reference is opt-in

Modifying the bindings and checking the result through the framework immediately can require a
source reference. Switch that path on only through an explicit option.

For example:

```bash
# .NET
dotnet test -p:ZLinkUseBindingsSource=true

# Java/Kotlin
./gradlew test -Pzlink.useLocalBindings=true

# C++
cmake -S framework/languages/cpp -B build \
  -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON
```

Settle the option names against each language's existing conventions when implementing. What
matters is that the default is not a source reference.

### 2.3 CI must verify the package boundary

CI needs at least one path that does this.

1. Build the bindings.
2. Produce the bindings package or install artifact.
3. Have the framework restore, build, and test from that artifact alone, without referencing the
   bindings source tree.

This verification is what finds a file missing from the real distribution package quickly.

## 3. The current state of the repository

This section reflects the code structure as it stands. Re-check it whenever the build files change.

### 3.1 .NET

The .NET framework project currently has a path that references the bindings `.csproj` directly.

The representative paths:

- `framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj`
- `bindings/dotnet/src/Zlink/Zlink.csproj`

`bindings/dotnet/src/Zlink/Zlink.csproj` carries the NuGet package metadata and the native runtime
assets. So for .NET the framework should default to `PackageReference` rather than
`ProjectReference`.

The recommended structure:

- Default: `PackageReference Include="Zlink"`
- Local verification: put the `.nupkg` from `dotnet pack` into a repo-local NuGet feed.
- Exception path: reference the bindings project directly only when `ZLinkUseBindingsSource=true`.

For example:

```bash
dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o .artifacts/nuget

dotnet restore framework/languages/dotnet/Zlink.Framework.sln \
  --source .artifacts/nuget \
  --source https://api.nuget.org/v3/index.json
```

A NuGet package can carry the managed assembly and the native runtime assets together, which makes
it safer than copying a DLL or native library into the framework directory.

### 3.2 Java/Kotlin

The Java/Kotlin framework currently has both Maven coordinates and a Gradle composite build.

What is visible today:

- The framework build uses the `systems.zlink:zlink` Maven coordinates.
- `framework/languages/java/settings.gradle.kts` configures a GitHub Packages repository.
- In the same file, when `zlink.useLocalBindings` is true, `../../../bindings/java` is pulled in
  with `includeBuild` and `systems.zlink:zlink` is substituted with the local project.

The direction is right, but the default needs adjusting. An ordinary framework build should
reference the Maven artifact, and the bindings source composite build should be switched on
explicitly.

The recommended structure:

- Default: reference the `systems.zlink:zlink:<version>` Maven artifact
- Local verification: publish the bindings artifact to a repo-local Maven repository or
  `mavenLocal()`
- Exception path: `includeBuild("../../../bindings/java")` only when `-Pzlink.useLocalBindings=true`

For example:

```bash
# Publish the bindings artifact to the local Maven repository.
cd bindings/java
./gradlew publishToMavenLocal

# The framework builds through the Maven artifact.
cd ../../framework/languages/java
./gradlew test -Pzlink.useLocalBindings=false
```

A repo-local Maven repository depends less on the state of the user machine's global `~/.m2`.

```bash
cd bindings/java
MAVEN_REPOSITORY_URL=file://$PWD/../../.artifacts/maven ./gradlew publish
```

Match the actual environment variable and Gradle property names to the build scripts when
implementing. The point is to stop the framework from including the bindings source tree by
default.

The Kotlin framework uses the same JVM artifact as the Java framework. There is no need for a
separate bindings copy path for Kotlin alone.

### 3.3 Node.js

The Node framework currently references the bindings package by file path in places.

The representative structures:

- `@zlink-systems/zlink: file:../../../../../bindings/node` in
  `framework/languages/node/packages/framework/package.json`
- direct `bindings/node/dist/index.d.ts` paths in several `tsconfig.json` files
- direct use of `../../../bindings/node/node_modules/typescript/bin/tsc` in the build script

This is fast for local development but verifies nothing about the package boundary. For Node,
installing a tarball produced by `npm pack` as a local package is better.

The recommended structure:

- Default: a `@zlink-systems/zlink` version dependency
- Local verification: install the `.tgz` from `npm pack`
- Exception path: `file:` or a workspace link only while developing the bindings

For example:

```bash
cd bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/npm

cd ../../framework/languages/node
npm install ../../../.artifacts/npm/zlink-systems-zlink-*.tgz
npm test
```

`npm link` depends heavily on machine-local state, so it is not used as a reproducible verification
path. Use it for manual debugging only, if at all.

If the Node framework package is also to be published later, the framework itself should have a
path where `npm pack` runs and the samples and e2e install the packed artifact. That is what
catches omissions in `files`, `exports`, `types`, and the native prebuilds.

### 3.4 C++

The C++ framework currently has a path where `framework/languages/cpp/CMakeLists.txt` includes the
bindings C++ project directly with `add_subdirectory`. At the same time, the framework install
consumer test verifies the flow that consumes installed package configs through
`find_package(zlink_framework_cpp CONFIG REQUIRED)` and
`find_package(zlink_stream_connector_cpp CONFIG REQUIRED)`.

For C++, a CMake package install prefix is better than a plain binary copy, because CMake can
manage the headers, libraries, imported targets, transitive dependencies, and runtime paths
together.

The recommended structure:

- Default: reference the bindings C++ package with `find_package(zlink_cpp CONFIG REQUIRED)`
- Local verification: install the C++ bindings to an install prefix and configure the framework
  with `CMAKE_PREFIX_PATH`
- Exception path: `add_subdirectory` source mode only while developing the C++ bindings

For example:

```bash
cmake -S bindings/cpp -B .artifacts/build/bindings-cpp \
  -DCMAKE_INSTALL_PREFIX=$PWD/.artifacts/install/zlink-cpp \
  -DZLINK_CPP_BUILD_TESTS=OFF \
  -DZLINK_CPP_BUILD_SAMPLES=OFF

cmake --build .artifacts/build/bindings-cpp
cmake --install .artifacts/build/bindings-cpp

cmake -S framework/languages/cpp -B .artifacts/build/framework-cpp \
  -DCMAKE_PREFIX_PATH=$PWD/.artifacts/install/zlink-cpp \
  -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=OFF

cmake --build .artifacts/build/framework-cpp
ctest --test-dir .artifacts/build/framework-cpp --output-on-failure
```

A vcpkg registry or a Conan package is possible in the longer term. But the presence of a Conan
workflow and a vcpkg overlay in the repository must not be taken to mean that the packages are
already published to public ConanCenter or the official vcpkg ports. Confirm publication to the
official registries separately at release time.

## 4. External versus local registries

A package reference in this document does not necessarily mean a public registry.

| Kind | Purpose | Examples |
|------|------|----|
| public registry | distribution to users | NuGet.org, Maven Central, npm, ConanCenter |
| private registry | distribution inside an organization | GitHub Packages, an in-house NuGet/Maven/npm/Conan registry |
| repo-local feed | local and CI verification | `.artifacts/nuget`, `.artifacts/maven`, `.artifacts/npm`, an install prefix |
| source mode | bindings development | ProjectReference, Gradle includeBuild, npm file link, CMake add_subdirectory |

Before formal publication, a repo-local feed and a private registry are enough. What matters is
having a verification path where the framework consumes *the packaged result*, not the bindings
source.

## 5. Keeping WSL and Windows local packages separate

Local distribution package verification has to treat WSL and Windows separately. They differ not
only in file path conventions but in the format of the native runtime files.

| Environment | Representative native file | Caution |
|------|------------------|--------|
| WSL/Linux | `libzlink.so` | Use the Linux package and install prefix. |
| Windows | `zlink.dll` | Use the Windows package and install prefix. |

Native artifacts built in one environment must not be used as-is to verify the framework in the
other. Verifying the Windows .NET or Node framework against a package containing only a `.so` built
under WSL, for instance, does not confirm the conditions a real user installs under.

### 5.1 Recommended directory layout

Split local artifacts by environment.

```text
.artifacts/
  wsl/
    nuget/
    maven/
    npm/
    install/
      zlink-cpp/
  windows/
    nuget/
    maven/
    npm/
    install/
      zlink-cpp/
```

With this split, WSL and Windows artifacts do not mix even when verifying the same version.

### 5.2 Verification run under WSL

Under WSL, build the packages containing the Linux native runtime and then verify the framework.

For example:

```bash
ZLINK_LOCAL_PACKAGE_ROOT=.artifacts/wsl

dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o "$ZLINK_LOCAL_PACKAGE_ROOT/nuget"

cd bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/wsl/npm
```

C++ gets its own WSL install prefix.

```bash
cmake -S bindings/cpp -B .artifacts/wsl/build/bindings-cpp \
  -DCMAKE_INSTALL_PREFIX=$PWD/.artifacts/wsl/install/zlink-cpp

cmake --build .artifacts/wsl/build/bindings-cpp
cmake --install .artifacts/wsl/build/bindings-cpp
```

### 5.3 Verification run on Windows

On Windows, keep separate PowerShell-based scripts. This path builds the packages containing the
Windows native runtime and verifies the Windows framework build.

For example:

```powershell
$env:ZLINK_LOCAL_PACKAGE_ROOT = ".artifacts/windows"

dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj `
  -c Release `
  -o "$env:ZLINK_LOCAL_PACKAGE_ROOT/nuget"

Push-Location bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/windows/npm
Pop-Location
```

C++ uses a separate build directory matching the Windows generator and toolchain.

```powershell
cmake -S bindings/cpp -B .artifacts/windows/build/bindings-cpp `
  -DCMAKE_INSTALL_PREFIX="$PWD/.artifacts/windows/install/zlink-cpp"

cmake --build .artifacts/windows/build/bindings-cpp --config Release
cmake --install .artifacts/windows/build/bindings-cpp --config Release
```

### 5.4 Script layout

Per-environment entry points are safer for the scripts as well.

```text
scripts/local-package/build-wsl.sh
scripts/local-package/native/sync-local-core-libs.sh
scripts/local-package/<lang>/build-wsl.sh
```

The policy is shared, but the commands, path separators, native runtimes, and CMake generators
differ. How to use the current local package build scripts lives in
[`scripts/local-package/README.md`](../../scripts/local-package/README.md).

Keep the local distribution scripts in `scripts/local-package/` and nowhere else. Under
`bindings/<lang>/`, keep only package metadata and build rules — NuGet, Maven, npm, CMake install.
Duplicating executable scripts into each bindings directory makes it easy for the WSL/Windows
artifact locations and verification policies to diverge.

Scripts that copy a core release artifact or a local core build result into the bindings workspace
are also managed under `scripts/local-package/native/`. The synchronization scripts that used to
live under `bindings/` or `core/tools/` are not kept; automation and documentation call the new
paths directly.

The .NET binding uses `bindings/dotnet/native/<rid>/` as the canonical location for native
libraries. Those files are placed into the `runtimes/<rid>/native/` layout only when building the
NuGet package. So `bindings/dotnet/runtimes/` is not refreshed as a separate source input.

## 6. On committing binaries to the Git repository

Committing built binaries to Git and having the framework reference those files is not used as a
default strategy.

The problems:

- The number of per-platform files grows.
- It is hard to trace who built a binary and when.
- Failing to refresh a binary after a source change is hard to notice from a git diff alone.
- Package metadata verification is skipped.

A vendored package cache for offline development is a possible exception. Even then, do not copy a
bare DLL or JAR — store package units such as a NuGet package, a Maven artifact, an npm tarball, or
a CMake install archive. Document the checksums and the procedure that produced them.

## 7. Recommended migration order

### 7.1 Step 1: inventory the current direct references

Find every direct bindings source reference in each language.

```bash
rg -n "bindings/(dotnet|java|node|cpp)|ProjectReference|includeBuild|file:|add_subdirectory" \
  framework/languages
```

Use that inventory to separate the default build path from the source mode path.

### 7.2 Step 2: fix the bindings package build commands

Give each bindings one command that produces its local package.

| Language | Artifact |
|------|--------|
| .NET | `.nupkg` |
| Java/Kotlin | a Maven repository layout |
| Node.js | `.tgz` |
| C++ | a CMake install prefix or a package archive |

The commands must behave the same way in CI and locally.

### 7.3 Step 3: switch the framework's default build to package mode

Remove the source reference from the default in the framework build files. Provide options that
take a package feed location or a version instead.

For example:

```bash
ZLINK_BINDING_VERSION=8.6.3
ZLINK_LOCAL_PACKAGE_ROOT=.artifacts
```

Settle the option names against each language's tooling, but keep the meaning the same.

### 7.4 Step 4: keep source mode as an explicit option

Bindings developers need source mode. It just has to work only when switched on explicitly.

For example:

```bash
ZLINK_USE_BINDINGS_SOURCE=true
```

The rule that the framework uses only the bindings' public API holds in source mode too. Do not
route around internal or private members with reflection or friend declarations.

### 7.5 Step 5: add package boundary CI

Add verification stages with these names to CI.

```text
build-bindings-package
build-framework-from-bindings-package
run-framework-tests
```

Source mode must be off in these stages. With source mode on, a package omission cannot be caught.

## 8. Final recommendation per language

| Language | Apply first | Long-term direction |
|------|------------------------|-----------|
| .NET | local NuGet feed, `PackageReference` as the default | NuGet.org or a private NuGet |
| Java/Kotlin | `zlink.useLocalBindings=false` as the default, verified against a local Maven feed | GitHub Packages or Maven Central |
| Node.js | drop the `file:` default reference, verify by installing the packed tarball | the npm registry or a private npm registry |
| C++ | drop the `add_subdirectory` default, consume the CMake install package | a vcpkg registry or a Conan package |

## 9. Definition of done

The migration counts as complete when all of these hold.

- The framework's default build does not reference the bindings source tree.
- After rebuilding the bindings package, the framework builds from that package alone.
- Source mode is switched on only through an explicit option.
- The samples and e2e are verified in package mode at least once.
- WSL and Windows verification artifacts do not mix.
- CI fails when a native runtime, header, type declaration, or piece of metadata is missing from a
  package.
- Neither the documentation nor the build scripts assume that the packages are already published to
  an external registry.
