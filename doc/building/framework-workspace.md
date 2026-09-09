# Framework Workspace Layout

This document collects in one place what the framework repository builds by default, what is
opt-in, how the samples and the bindings are referenced, and which presets open the C++ tree.
These decisions were fixed on 2026-09-09; when the layout changes, update this document first.

- Build script locations and the release path: [Build and release pipeline](./release-pipeline.md)
- Why bindings are consumed as packages: [Framework/bindings dependency boundary](./framework-bindings-dependency-boundary.ko.md)
- Version number rules: [Versioning policy](./versioning.md)

## 1. What the default build includes and excludes

The framework's default build, CI and release include only the **runtime libraries, unit tests and
the `cross-language` e2e host**. The following two are excluded from the default and run only
through their dedicated run scripts.

| Target | Location | How to run |
|---|---|---|
| Per-language scenario e2e | `framework/languages/<lang>/e2e/<name>/` | each `run_e2e.sh` / `run_e2e.ps1` |
| The seven samples (Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, TicTacToe, ZoneWorld) | `framework/languages/<lang>/samples/<name>/` | `samples/run_samples.sh` / `.ps1`, per-sample `run_sample.sh` / `.ps1`, Node `npm run sample` |

Per language, "excluded from the default" means the following.

| Language | Default build unit | How samples and e2e stay out |
|---|---|---|
| .NET | CI and release build only `Zlink.Framework.ci.slnf` (16 projects) | The developer `Zlink.Framework.sln` carries the sample projects so the IDE can open them, but CI and release use the slnf only. Each sample also opens through `samples/<name>/<name>.sln` |
| Node.js | `npm run verify:ci` (workspace `packages/*`) | Samples are standalone packages outside the workspace. `npm run test:samples`, `npm run lint:samples` and `npm run verify:samples` run only in `verify:release` and `framework-gate.sh` |
| Java/Kotlin | Modules of the root `settings.gradle.kts` | Samples are the `samples/` composite build. The root build registers the composite without running its tasks, and CI and release skip the registration with `-Pzlink.includeSamples=false` |
| C++ | `ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF`, `ZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF`, `ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=ON` by default | The run scripts pass the option they need as `ON` and build in the same build tree |

## 2. The two sample modes

There is one sample source; it behaves in one of two modes depending on where it is opened.

| Mode | Condition | Framework reference | Bindings reference |
|---|---|---|---|
| Developer mode | The sample sits inside the repository under `framework/languages/<lang>/samples/` | The repository's framework source (ProjectReference, composite build, workspace link, `add_subdirectory`) | The per-language default of §3 |
| User mode | The sample directory was copied outside the repository, or the override is given | The published framework package (`0.10.0`) | The published bindings package (`0.17.6`) |

The mode is detected automatically. The override exists only to check user mode from inside the
repository.

| Language | Detection | Force user mode | Copy out of the repository |
|---|---|---|---|
| .NET | `samples/Directory.Build.props` checks whether `../src/Zlink.Framework/Zlink.Framework.csproj` exists | `-p:ZLinkSampleUseLocalSource=false` | `scripts/local-package/dotnet/prepare-sample.sh <Sample> <dest>` (`.ps1` likewise) |
| Node.js | `samples/scripts/prepare-sample-dependencies.mjs` finds the parent workspace (`@zlink-systems/node-framework-workspace`) | `ZLINK_NODE_SAMPLES_PACKAGE_MODE=1` | Copy the sample directory (`samples/<Name>.Ts`) as is, then `npm install` |
| Java/Kotlin | `samples/gradle/zlink-sample-dependencies.settings.gradle.kts` finds the framework `settings.gradle.kts` above it | `-Pzlink.samples.packageMode=true` (`-Pzlink.frameworkVersion`, `-Pzlink.bindingsVersion` pin versions) | Copy the whole `samples/` directory |
| C++ | The sample `CMakeLists.txt` finds `../../CMakeLists.txt` and `framework/include/zlink/framework.hpp` | `-DZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE=ON` | Copy `samples/<Name>/`. Its `vcpkg.json` and `conanfile.txt` require `zlink` and `zlink-framework` |

C++ user mode is complete once the `zlink-framework` vcpkg port and Conan recipe install from the
public assets. Today the port and recipe live in the repository overlay (`vcpkg/ports/`,
`framework/languages/cpp/packaging/conan/`); until the public assets are republished and the
checksums updated, C++ samples build only in developer mode inside the repository.

## 3. Bindings: package by default, source as opt-in

The framework references the bindings as **published packages**. Source references are switched on
only while changing binding sources and checking them through the framework. A source reference
cannot be combined with sample user mode.

| Language | Default (package) | Source switch |
|---|---|---|
| .NET | NuGet `Zlink` (the local feed under `ZLINK_LOCAL_PACKAGE_ROOT/nuget` or `.artifacts/{wsl,windows}/nuget` is added automatically) | `-p:ZLinkUseBindingsSource=true` |
| Node.js | npm `@zlink-systems/zlink` | `ZLINK_NODE_USE_BINDINGS_SOURCE=1`, or `npm run use:bindings-source` / `npm run use:bindings-package` |
| Java/Kotlin | Maven `systems.zlink:zlink` | `ZLINK_JAVA_BINDINGS_SOURCE=<path to bindings/java>` |
| C++ | The installed `zlink_cpp` CMake package (`find_package(zlink_cpp <version> EXACT)`) | `-DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON` |

Unreleased binding packages are built with the [local package guide](../../scripts/local-package/README.ko.md)
and pointed to with `ZLINK_LOCAL_PACKAGE_ROOT`.

## 4. C++ presets and dependency setup

The C++ framework opens through the configure presets in `framework/languages/cpp/CMakePresets.json`.
Preset names follow the IDE or platform; the option bundles are `dev` and `ci`.

| Preset | Use | Generator | Notes |
|---|---|---|---|
| `vs2022` | Visual Studio 2022 | Visual Studio 17 2022 | CMake generates the `.sln`; there is no separate generator script |
| `windows-ninja` | Windows terminal, VS Code, CLion | Ninja | |
| `linux-ninja` | Linux, WSL | Ninja | |
| `macos-ninja` | Apple Silicon Mac | Ninja | `arm64-osx` triplet. Intel Mac is unsupported from Core up |
| `dev` | Repository development | Ninja | samples, e2e, tests, foundation tests and cross-language all `ON` |
| `ci` | CI and release verification | Ninja | samples and e2e `OFF`, tests and cross-language `ON` |

Every preset uses the vcpkg toolchain under `VCPKG_ROOT` and the repository overlay ports
(`vcpkg/ports`). One bootstrap script prepares them.

```bash
source scripts/dev/bootstrap-cpp.sh        # clones vcpkg into ~/.cache/zlink/vcpkg and sets VCPKG_ROOT and VCPKG_OVERLAY_PORTS
cmake --preset linux-ninja -S framework/languages/cpp
cmake --build --preset linux-ninja
```

```powershell
. scripts/dev/bootstrap-cpp.ps1
cmake --preset vs2022 -S framework/languages/cpp
```

Add `--conan` for Conan. Conan is used from a standalone sample (`conanfile.txt`): `conan install`
followed by `cmake --preset conan`. The repository build defaults to the vcpkg manifest.

Each sample directory carries presets with the same names, so one sample can be opened alone in an
IDE.

## 5. WSL and Windows

Commands are the same on WSL and Windows. The only differences are the script extension
(`.sh`/`.ps1`) and the preset name (`linux-ninja` versus `windows-ninja` or `vs2022`). Local package
outputs are split into `.artifacts/wsl/` and `.artifacts/windows/` and never mix.
