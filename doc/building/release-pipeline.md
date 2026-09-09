[English](./release-pipeline.md) | [한국어](./release-pipeline.ko.md)

# Build and Release Pipeline at a Glance

This page summarizes **where** (GitHub Actions workflow), **how** (trigger and authentication), and
**to where** (public channel) each zlink component is built and released. Procedures live in the
[packaging guide](./packaging.md); accounts and secrets in [release accounts](./release-accounts.md).
Every release runs in GitHub Actions; nothing is published from a local machine.

## 1. Release path per component

| Component | Artifact | Channel | Workflow | Trigger | Auth |
| --- | --- | --- | --- | --- | --- |
| Core (`core/`) | 5 native archives, source tarball, checksums, provenance | GitHub Release `core/vX.Y.Z` | `build.yml` | create the `core/vX.Y.Z` tag, then `workflow_dispatch` on that ref | `GITHUB_TOKEN` |
| Core (Conan) | recipe | ConanCenter | none (PR) | PR to `conan-io/conan-center-index` `recipes/zlink/` | GitHub account |
| Core (vcpkg) | port | microsoft/vcpkg | none (PR) | PR to `microsoft/vcpkg` `ports/zlink/` + `versions/` | GitHub account |
| Binding C++ | source archive | GitHub Release `cpp/vX.Y.Z` | `bindings-release.yml` | `cpp/v*` tag or dispatch | `GITHUB_TOKEN` |
| Binding Node | `@zlink-systems/zlink` (with linux-x64 prebuild) | npm | `bindings-release.yml` | `node/v*` tag or dispatch | npm Trusted Publishing (OIDC, provenance) |
| Binding Java | `systems.zlink:zlink`, `zlink-ext-netty` | Maven Central, GitHub Packages | `bindings-release.yml` | `java/v*` tag or dispatch | `MAVEN_CENTRAL_*`, `SIGNING_*` (GPG) |
| Binding .NET | `Zlink` nupkg (+snupkg) | nuget.org | `release-dotnet.yml` (target `binding`) | `dotnet/v*` tag or dispatch | nuget Trusted Publishing (`NuGet/login`, policy `zlink-dotnet-release`) |
| Framework C++ | source archive + sha256 | GitHub Release `framework/vA.B.C` | `framework-release.yml` | `framework/v*` tag or dispatch | `GITHUB_TOKEN` |
| Framework Node | 8 `@zlink-systems/*` packages | npm | `framework-release.yml` | same | npm Trusted Publishing (registered per package) |
| Framework JVM | 13 `systems.zlink:zlink-framework-*` artifacts (incl. Kotlin) | Maven Central | `framework-release.yml` | same | `MAVEN_CENTRAL_*`, `SIGNING_*` |
| Framework .NET | 9 packages (`Zlink.Framework*`, `Zlink.HttpClient`, `Zlink.Stream.Connector`, ...) | nuget.org | `release-dotnet.yml` (target `framework`) | `framework/v*` tag or dispatch | nuget Trusted Publishing |
| Documentation site | mkdocs static site | GitHub Pages | `docs.yml` | push to `main` (doc paths) | `GITHUB_TOKEN` |

Python, Go, and Rust bindings have jobs in `bindings-release.yml` but are outside the public release
scope. `core-conan-release.yml` is a legacy workflow for a private Conan remote and has no secrets.

## 2. Where the build scripts and tools live

| Target | Local entry point | Build in CI / release | Output |
| --- | --- | --- | --- |
| Core | `scripts/build-core.sh dev\|release\|release-gate` (trees `core/build-dev`, `core/build-release`). Direct CMake: [build guide](./build-guide.md), [CMake options](./cmake-options.md) | the platform jobs of `build.yml` build `core/` with CMake and archive `core/dist/<platform>/` | GitHub Release `core/vX.Y.Z` assets |
| Core local prefix | `scripts/local-package/core/fetch-release.sh --version V --platform P` (release archive → `~/.cache/zlink/core/<V>/<P>`), `scripts/gate/materialize-local-core-prefix.sh` (dev build → prefix) | every binding and framework job uses the same `fetch-release.sh` | `~/.cache/zlink/core/`; CI uses `.artifacts/core-release/` |
| Bindings (7 languages) | `scripts/local-package/build-wsl.sh [cpp\|node\|java\|dotnet\|python\|go\|rust\|c]` (Windows: `build-windows.ps1`); per-language tests `bindings/<lang>/tests/run_tests.sh` | the "Build and test" step of each `bindings-release.yml` / `release-dotnet.yml` job | `.artifacts/wsl/{npm,nuget,maven,install}/` |
| Framework C++ | presets in `framework/languages/cpp/CMakePresets.json`, Windows `build-windows.ps1`; samples `samples/run_samples.sh`; scenario e2e `e2e/<name>/run_e2e.sh` (opt-in) | `framework-release.yml` only produces the source archive | GitHub Release `framework/vA.B.C` |
| Framework .NET | `dotnet build framework/languages/dotnet/Zlink.Framework.sln` (needs `ZLINK_LOCAL_PACKAGE_ROOT`); samples `samples/<name>/<name>.sln` | `framework-dotnet.yml` (verification), `release-dotnet.yml` target `framework` (pack and push) | nuget.org |
| Framework JVM | `framework/languages/java/gradlew assemble` (`test` for tests); Central bundle via `scripts/upload-central-bundle.sh` | `framework-release.yml` `release-java` | Maven Central |
| Framework Node | `npm ci && npm run build` in `framework/languages/node`; the http-client local tarball via `scripts/local-package/http-client/build-wsl.sh node`; gate `npm run verify:ci`, release gate `verify:release` | `framework-node.yml` (verification), `framework-release.yml` `release-node` (pack and publish) | npm |
| Version sync | `scripts/local-package/sync-version.py --write` or `build-wsl.sh --sync-versions` / `--verify-versions` | every release job verifies tag, `VERSION`, and manifests agree | — |
| Integrated gates | `scripts/gate/{rebuild-dev,framework-gate,bindings-gate,cross-language-e2e}.sh <tag>` ([README](../../scripts/gate/README.md)) | the CI workflows cover the same scope as a platform matrix | `zlink-work/gates/<tag>/` |
| Performance | `scripts/perf/perf-ticket.sh submit …` (ticket queue, `perf-queue-runner.sh`) | never in CI | `.artifacts/perf-queue/`, `doc/perf/perf/` |
| CI helpers | `.github/actions/msvc-env` (Windows MSVC environment), `scripts/ci/dotnet-test-retry.sh` | called by the workflows | — |
| Workflows | `.github/workflows/`: `build.yml`, `bindings-release.yml`, `release-dotnet.yml`, `framework-release.yml`, `framework-node.yml`, `framework-dotnet.yml`, `docs.yml`, (legacy) `core-conan-release.yml` | — | — |

## 3. Order

Number rules and compatibility are owned by the [versioning policy](./versioning.md). `VERSION` (Core) and `BINDINGS_VERSION` are the version files; `scripts/local-package/sync-version.py
--write` propagates pins across the repository. The order is always **Core → 4 bindings → 4
frameworks**; framework packages pin the published binding packages, so the order never inverts.

1. Core: update `VERSION`, add a `core/CHANGELOG.md` section, push `core/vX.Y.Z`, dispatch `build.yml`
   on that ref. Check the release assets, `checksums.txt`, and `release-provenance.txt`.
2. Bindings: after `sync-version.py --write`, push `cpp/v`, `node/v`, `java/v` (→
   `bindings-release.yml`) and `dotnet/v` (→ `release-dotnet.yml`). Each job verifies that the
   checkout's `VERSION` and Core sources match the tag exactly, fetches the Core release archive
   (`scripts/local-package/core/fetch-release.sh`), then builds, tests, and packs. CI never rebuilds Core.
3. Framework: one `framework/vA.B.C` tag runs `framework-release.yml` (C++, Node, JVM) and
   `release-dotnet.yml` (.NET). Both wait (up to 45 minutes) until the pinned binding package is really
   served by its registry.
4. Conan and vcpkg: update `core/packaging/conan/conandata.yml` and
   `vcpkg/ports/zlink/portfile.cmake` with the source tarball hashes and open PRs upstream. Draft
   bodies are in `doc/building/pr-drafts/`.

```bash
gh workflow run build.yml --ref core/v0.17.5 -f libzlink_version=0.17.5
gh workflow run bindings-release.yml -f target=node -f version=0.17.5 -f create_release=true -f publish_registry=true
gh workflow run release-dotnet.yml -f target=binding -f version=0.17.5
gh workflow run framework-release.yml -f version=0.10.0 -f publish_registry=true
gh workflow run release-dotnet.yml -f target=framework -f version=0.10.0
```

## 4. How each channel works

- **GitHub Release**: `softprops/action-gh-release` creates the per-tag release and uploads assets.
  Core release notes are extracted from the matching `core/CHANGELOG.md` section.
- **npm**: published with OIDC, no token (`npm publish --provenance --access public`). Every package
  needs a Trusted Publisher on npmjs.com (org `zlink-systems`, repo `zlink`, workflow filename):
  the binding uses `bindings-release.yml`, the 8 framework packages use `framework-release.yml`.
  `setup-node`'s `registry-url` is not used (it writes an empty `_authToken` and causes 404s).
- **Maven Central**: Gradle builds a signed bundle (in-memory GPG, sources, javadoc) and
  `*/scripts/upload-central-bundle.sh` uploads it to the Central Portal Publisher API with
  `publishingType=AUTOMATIC`; it goes public without a click once validation passes. Poll
  `POST https://central.sonatype.com/api/v1/publisher/status?id=<deployment>`.
- **nuget.org**: pushed with the short-lived token from `NuGet/login@v1`. Policy
  `zlink-dotnet-release` is bound to the workflow filename `release-dotnet.yml`, so no other workflow
  can push. Package IDs are `Zlink` and `Zlink.*` (`Systems.` is reserved on nuget.org). The snupkg is
  accepted only after package validation, so it is retried up to 30 times and never fails the release.
- **ConanCenter and vcpkg**: recipes only, no binaries. Consumers download the source tarball from the
  GitHub Release, verify the hash, and build locally. Upstream maintainers merge the PRs.

## 5. Registry propagation and retries

Right after a publish, registries show metadata before serving the file (npm tarball about 3 minutes,
nuget flat container after validation). The framework workflows and framework CI wait for the pinned
binding tarball/nupkg to return 200, once a minute for up to 45 minutes. Already-published versions are
skipped, so re-dispatching the same version after a partial failure publishes only what is missing.

## 6. Post-release checks

```bash
curl -sI https://registry.npmjs.org/@zlink-systems/zlink/-/zlink-0.17.5.tgz | head -1
curl -sI https://api.nuget.org/v3-flatcontainer/zlink/0.17.5/zlink.0.17.5.nupkg | head -1
curl -sI https://repo1.maven.org/maven2/systems/zlink/zlink/0.17.5/zlink-0.17.5.pom | head -1
gh release view core/v0.17.5 --json assets -q '.assets[].name'
```

## 7. Supported platforms and runtime requirements

| Platform | Core release | Node prebuild | .NET runtimes | Notes |
| --- | --- | --- | --- | --- |
| linux-x64 | ✅ | ✅ | ✅ | release runner ubuntu-24.04, needs glibc ≥ 2.38 |
| linux-arm64 | ✅ | source build | Core archive | |
| macos-arm64 | ✅ | source build | Core archive | |
| windows-x64 | ✅ | source build | Core archive | archive bundles the OpenSSL DLLs |
| windows-arm64 | ✅ | source build | Core archive | |
| macos-x64 (Intel) | ❌ | ❌ | ❌ | unsupported from Core up (decided 2026-09-09) |

"Source build" means the addon is compiled at install time against the Core release archive named by
`ZLINK_CORE_SOURCE=release` and `ZLINK_CORE_PACKAGE_PREFIX`.

## 8. CI (verification) workflows

Separate from releases, these run on `main` pushes and PRs. Framework CI uses only the published
binding packages and Core release archives, and includes only the `cross-language` e2e (per-language
scenario e2e is opt-in through each `run_e2e.sh`). The seven samples (Bingo, DeliveryDispatch, GameQuest,
ShoppingMall, SupportChat, TicTacToe, ZoneWorld) are likewise outside the framework build, CI and
releases; they are verified only by the local gate (`scripts/gate/framework-gate.sh`), each language's
`samples/run_samples.sh`, and Node `npm run test:samples`.

| Workflow | Scope | Matrix |
| --- | --- | --- |
| `framework-node.yml` | Node framework gate, Chromium STREAM e2e, Node↔.NET cross-language smoke | 5 platforms × Node 20/22 |
| `framework-dotnet.yml` | .NET framework unit, contract, and stream connector tests | 5 RIDs × net8.0 Debug / net10.0 Release |
| `build.yml` | Core build and verification (also the release workflow) | 5 platforms |
| `docs.yml` | documentation site build and deploy | ubuntu |

Windows jobs set up MSVC through `.github/actions/msvc-env` (composite, no Node runtime). .NET unit
tests that fail from scheduler noise on shared runners are rerun once, failed tests only, by
`scripts/ci/dotnet-test-retry.sh`; test tolerances are never widened.

## 9. Local vs. workflow responsibilities

- Local: version bump (`sync-version.py`), CHANGELOG and release notes, tag push, dispatch, result
  checks, Conan/vcpkg recipe updates with local verification (`conan create`, `vcpkg install`).
- Never local: `npm publish`, `dotnet nuget push`, Central uploads, signing. Secrets exist only as
  GitHub repository secrets; documents and logs record names, never values.

## 10. Related records

- [Release accounts and secrets](./release-accounts.md)
- [Packaging procedure](./packaging.md)
- [Release notes](./release-notes/)
- [Release preparation records](./release-prep/) — workflow fixes on 2026-09-08 and 09
- [PR drafts](./pr-drafts/) — ConanCenter #30935, vcpkg #53846
