[English](./versioning.md) | [한국어](./versioning.ko.md)

# Versioning Policy

This document owns the version-number rules and compatibility relations of the zlink components;
other documents only link here. Decided on 2026-09-09.

## 1. Numbers per component

| Component | Format | Source of truth | Published as |
| --- | --- | --- | --- |
| Core (`core/`) | `MAJOR.MINOR` | `VERSION` (`LIBZLINK_VERSION`) | `MAJOR.MINOR.0` |
| Bindings (C++, Node, Java, .NET; also Python, Go, Rust) | `CORE_MAJOR.CORE_MINOR.N` | `BINDINGS_VERSION` | as is |
| Framework (C++, Node, JVM, .NET) | `MAJOR.MINOR.HOTFIX` | `FRAMEWORK_VERSION` (`ZLINK_FRAMEWORK_VERSION`) | as is |

- **Core has two components.** The third is never used. Any Core change, bug fixes included, bumps
  `MINOR`. npm, NuGet, and Maven require three-part semver, so `.0` is appended when publishing.
- **A binding number says which Core it matches.** The first two components are the `MAJOR.MINOR` of
  the Core it embeds or depends on exactly; `N` counts binding-only fixes. When Core moves to `0.18`
  the bindings restart at `0.18.0`. The four bindings share one `BINDINGS_VERSION`, so a fix in one
  language republishes all four under the same number.
- **Framework has three components.** Features and contract changes bump `MINOR`; fixes without new
  features and republishes that only move the binding pin bump `HOTFIX`, which resets to 0 when
  `MINOR` moves.
- Before 1.0, a `MINOR` bump promises no compatibility.

## 2. Compatibility rules

- Binding `X.Y.*` matches Core `X.Y` only; never combine it with another Core.
- Framework `A.B` pins exactly the binding `X.Y.N` named in its release notes; installing the
  framework installs that binding.
- After 1.0: within one `MAJOR`, Core's public C API/ABI (`core/include/**`,
  `core/src/libzlink.vers`) and the public APIs of bindings and framework do not break. Breaking
  changes bump `MAJOR`.

## 3. Release order and tags

| Step | Tag | Workflow | Precondition |
| --- | --- | --- | --- |
| 1 | `core/vX.Y.0` | `build.yml` (dispatched on the tag ref) | `VERSION` updated, `core/CHANGELOG.md` section |
| 2 | `cpp/`, `node/`, `java/`, `dotnet/vX.Y.N` | `bindings-release.yml`, `release-dotnet.yml` | the checked-out `core/` sources must equal the Core tag exactly |
| 3 | `framework/vA.B.C` | `framework-release.yml`, `release-dotnet.yml` | the binding packages must be served by their registries |

Push at most three tags per push: GitHub creates no workflow event for a push with more than three
tags (observed with bindings 0.17.6 on 2026-09-09). Dispatching `bindings-release.yml` per language
is the safer path. The full procedure is in [the release pipeline](./release-pipeline.md).

## 4. How to change a number

- Core: edit `VERSION` only.
- Bindings: edit `BINDINGS_VERSION` only.
- Framework: edit `FRAMEWORK_VERSION` only; `sync-version.py --write` aligns the 107 files (.NET props, Node
  packages, samples and lockfile, Java/Kotlin build files, C++ vcpkg/Conan manifests) listed in the script's registry.
  Planned: .NET (MSBuild), Gradle and CMake will read `FRAMEWORK_VERSION` at build time, shrinking the sync
  set to the npm, vcpkg and Conan manifests plus the standalone-sample defaults (about 30 files); those
  tools read JSON/text verbatim and need literal versions.
- Then `python3 scripts/local-package/sync-version.py --write` aligns manifests, pins, headers, and
  snapshots at once, and `scripts/local-package/build-wsl.sh --verify-versions` checks for missing
  pins. Never hunt for pins by hand.

## 5. The 1.0 plan

The last 0.x published now is framework 0.11.0 (user decision, 2026-09-09). Everything after it
is collected and shipped at once, with no intermediate release, as Core `1.0`, bindings `1.0.0` and
framework `1.0.0`; there is no intermediate binding release such as 0.17.7. Conditions for 1.0:

- Public C API/ABI frozen (`libzlink.vers`), release archives and green CI for the five supported
  platforms (linux-x64, linux-arm64, macos-arm64, windows-x64, windows-arm64).
- Public APIs of the four bindings frozen and the binding performance campaign on machine B
  judged complete.
- The framework aligns with the same 1.0 as Core and the bindings. The order stays Core →
  bindings → framework ([release pipeline](./release-pipeline.md) §4), all three tags on the same
  day.
- 1.0 refreshes the C++ public assets (source archives) and the checksums of the vcpkg ports and
  Conan recipes.

## 6. History

| Date | Decision |
| --- | --- |
| 2026-09-09 | Core `MAJOR.MINOR`, binding `CORE_MAJOR.CORE_MINOR.N`, framework `MAJOR.MINOR.HOTFIX` decided. The existing Core 0.17.5 predates the policy and is cleaned up at 1.0. Bindings 0.17.6 is the first case of republishing all four for a Node-only fix. The next framework is 0.11.0 (the .NET admission fix and the binding 0.17.6 pin). |
