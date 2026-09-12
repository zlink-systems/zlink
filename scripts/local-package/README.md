[English](./README.md) | [한국어](./README.ko.md)

# Local package

This directory is the path for packaging a Core release and the first-party bindings without
publishing to an external registry. The Core version is owned by the root `VERSION`, each binding
package version by `bindings/<language>/VERSION`, and each Framework package version by
`framework/languages/<language>/VERSION`. The default behaviour is to download the
`core/v<VERSION>` release asset from GitHub, verify its checksum and provenance, and build the Core
prefix the bindings consume. Output goes under `.artifacts/wsl/` by default.

## Version synchronization

The root `VERSION` is the single source for the Core release, the public headers, and the native
payload version. Each binding's and Framework's `VERSION` is the single source for that language's
package release version. Package manager manifests and Framework binding dependency pins follow the
binding `VERSION` of the same language; the Core prefix, provenance, and versioned runtime follow
the root `VERSION`; Framework manifests and sample dependency pins follow the Framework `VERSION`
of the same language. The C binding ships together with Core, so it has no separate package version
and uses the root `VERSION`. Synchronize and verify through the official entry points below.

When changing a Core, binding package, or Framework package release version, edit only the owning
`VERSION` file and then run `--sync-versions`. Do not hunt down and edit the per-language
manifests, Framework dependencies, sample pins, or the sample runners' local Core package paths by
hand. After synchronizing, confirm that no pin was missed with `--verify-versions`, then build the
local packages.

```bash
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions
```

Ordinary local-package builds and cache hits check the versions and exit on a mismatch. After
changing a version file, run `--sync-versions` above explicitly. The Framework package versions
themselves and the per-language sample pins are part of the same synchronization.

## Full build

```bash
scripts/local-package/build-wsl.sh
```

That command does not build Core from source separately. It prepares the release Core named by the
root `VERSION`, then packages the C, C++, .NET, Go, Java, Node.js, Python, and Rust bindings in
turn — C at the Core version, the rest at their own `bindings/<language>/VERSION`.

When `bindings/` and `scripts/local-package/` are clean, the shared binding packages matching the
input hash are reused. On a cache miss all eight bindings are built. `.artifacts/wsl/` is a
per-worktree directory; only the binding package files are linked into the shared cache. Framework
packages and build directories stay per-worktree.

If the input paths above carry staged, unstaged, or untracked changes, or if the build settings or
compiler flags differ from the default Release, the build happens in `.artifacts/wsl-private/`. To
package only certain bindings, pass the language names.

```bash
scripts/local-package/build-wsl.sh dotnet java node
```

Use the commands below to query the cache key and tool versions, and to prune the cache. The
sharing policy is owned by
[development workflow §4.1](../../doc/principal/dev/development-workflow.md#41-shared-local-package-cache-content-addressed).

```bash
scripts/local-package/build-wsl.sh --cache-key
scripts/local-package/cache-prune.sh --keep 5 --dry-run
scripts/local-package/cache-prune.sh --keep 5
```

**A Core release is a precondition — there is no bypass (settled 2026-08-28).** Even when verifying
a Core source change, do not substitute a local build: cut the release first, then package through
this path. The procedure is fixed as follows.

```bash
# (1) Settle the root VERSION, then synchronize and verify
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions

# (2) Tag the release commit and push the tag
git tag core/v<VERSION> <release-commit> && git push origin core/v<VERSION>

# (3) Dispatch the build workflow at the tag ref — build.yml does not run on a tag push
GH_REPO=zlink-systems/zlink gh workflow run build.yml --ref core/v<VERSION> -f libzlink_version=<VERSION>

# (4) Confirm the release assets were produced
GH_REPO=zlink-systems/zlink gh release view core/v<VERSION>

# (5) Build the local packages (release download + checksum and provenance verification)
scripts/local-package/build-wsl.sh cpp dotnet java node
```

If the release does not exist yet, step (5) failing with a 404 is the expected outcome — finish
(2) through (4) first. The former `--core-source local` and `--core-prefix` bypasses and the
`core/build-wsl.sh` local core builder have been removed.

The Core local package uses this layout.

```text
.artifacts/wsl/install/zlink-core/<VERSION>/
  include/
  lib/libzlink.so
  lib/libzlink.so.0
  lib/libzlink.so.<VERSION>
  share/zlink/core-package-provenance.json
```

The release Core prefix is cached at `~/.cache/zlink/core/<VERSION>/linux-x64/` by default. If
provenance for the same version and platform is already present, neither the download nor the Core
build is repeated. To use a different location:

```bash
bash scripts/local-package/core/fetch-release.sh \
  --version <VERSION> \
  --platform linux-x64 \
  --cache-dir /absolute/path/zlink-core-cache
```

`<VERSION>` is the release/package version. The native runtime's SONAME is also produced as
`libzlink.so.0`, matching the same release line. External dependency versions are outside this
policy.

## Output per binding

- C: `.artifacts/wsl/c/zlink-c-<CORE_VERSION>.tar.gz`
- C++: `.artifacts/wsl/install/zlink-cpp/<CPP_BINDING_VERSION>/`
- .NET: `.artifacts/wsl/nuget/Zlink.<DOTNET_BINDING_VERSION>.nupkg`
- Go: `.artifacts/wsl/go/zlink-go-<GO_BINDING_VERSION>.tar.gz`
- Java: `.artifacts/wsl/maven/systems/zlink/zlink/<JAVA_BINDING_VERSION>/`
- Node.js: `.artifacts/wsl/npm/zlink-systems-zlink-<NODE_BINDING_VERSION>.tgz`
- Python: `.artifacts/wsl/python/zlink-<PYTHON_BINDING_VERSION>-*.whl` and a source archive
- Rust: `.artifacts/wsl/rust/zlink-<RUST_BINDING_VERSION>.crate`

Go's public module path is `zlink.systems/zlink`, keeping the release version and the import path
separate. Every binding package uses the `VERSION` runtime and public headers recorded in the Core
provenance.

## Windows native verification

On Windows too, the bindings use the release prefix rather than building Core from source first.
The commands below download and verify the Windows x64 Core release.

```powershell
$prefix = powershell -ExecutionPolicy Bypass -File scripts/local-package/core/fetch-release.ps1

# All of the C++/.NET/Java/Node bindings, or one language at a time
scripts/local-package/build-windows.ps1 -SyncVersions
scripts/local-package/build-windows.ps1 -VerifyVersions
scripts/local-package/build-windows.ps1 -CorePrefix $prefix
scripts/local-package/cpp/build-windows.ps1 -CorePrefix $prefix
scripts/local-package/dotnet/build-windows.ps1 -CorePrefix $prefix
scripts/local-package/java/build-windows.ps1 -CorePrefix $prefix
scripts/local-package/node/build-windows.ps1 -CorePrefix $prefix

# The .NET/Java/Node HTTP client local packages that Framework consumes
scripts/local-package/http-client/build-windows.ps1
```

The default prefix is `%LOCALAPPDATA%\zlink\core\<VERSION>\windows-x64\`. When an in-progress
Windows Core change is needed, use the existing `core/build/windows-x64/install/` as the local
source fallback input. Do not interchange WSL output and Windows output.

The conditions for a Windows local source fallback build match the Windows x64 job in
`.github/workflows/build.yml`: the Visual Studio 17 2022 x64 generator with `Release`,
`BUILD_SHARED=ON`, `BUILD_STATIC=ON`, `BUILD_TESTS=OFF`, and C++17, leaving `ENABLE_LTO` at its
default. The standard runtime for these conditions is the MSVC dynamic CRT (`/MD`), and the result
installs to `core/build/windows-x64/install/`.

In environments where the `msvcp140.dll` the JDK loads first collides with the C++ runtime of a
`/MD` Core under Java 22 FFM, build a separate `/MT` Core for Java verification in its own build
directory. Do not mix that variant into the common CI runtime or another binding's staged runtime;
record it as separate evidence in the Java plan document.

Windows package inputs and results use these paths.

- .NET: `ZLinkWindowsX64NativeRoot=<release-prefix>/bin`, result in
 `.artifacts/windows/dotnet/package/`
- C++: pass `<release-prefix>/` as `ZLINK_CPP_CORE_PACKAGE_PREFIX`; the CMake install result lands in
 `.artifacts/windows/cpp/package/`
- Go: place the release prefix's `bin/` runtime in `bindings/go/native/windows-x86_64/`; result in `.artifacts/windows/go/package/`
- Java: a version-only consumer that uses the release prefix's `bin/zlink.dll`
- Node.js: place the release prefix's `bin/zlink.dll` in `bindings/node/prebuilds/win32-x64/`; result in `.artifacts/windows/node/package/`
- Python: place the release prefix runtime at the wheel's `native/windows-x86_64/zlink.dll`; result in `.artifacts/windows/python/wheel/`
- Rust: place the release prefix runtime in the crate's `native/windows-x86_64/`; result in `.artifacts/windows/rust/`

When consolidating the Windows native packaging procedure, update these paths together with the
per-language version pinning. The current state of Windows performance runs and their failure
causes are owned by the individual measurement sheets and `log/` under
`doc/perf/perf/core-0.10.0/`.

## Core runtime synchronization

`native/sync-local-core-libs.sh` copies the `<VERSION>` runtime and public headers from the
verified Core prefix that `ZLINK_CORE_PACKAGE_PREFIX` names into the binding working directories.
Only when that environment variable is absent does it fall back to `core/build/lib` and
`core/include` as the local source.

```bash
scripts/local-package/native/sync-local-core-libs.sh
```

These files are inputs to the local package build. When cutting a release package, do not commit
the native files the script generates separately.
