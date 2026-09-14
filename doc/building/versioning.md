[English](./versioning.md) | [한국어](./versioning.ko.md)

# Version Policy

This document owns the version-number rules and compatibility relationships for zlink components. Other
documents should link here instead of redefining them. The numbering policy was decided on 2026-09-09;
per-language version ownership was adopted on 2026-09-10.

## 1. Component numbers and sources

| Component | Number format | Source file | Published form |
| --- | --- | --- | --- |
| Core (`core/`) | `MAJOR.MINOR` | `VERSION` (`LIBZLINK_VERSION`) | `MAJOR.MINOR.0` |
| Each binding | `CORE_MAJOR.CORE_MINOR.N` | `bindings/<language>/VERSION` (`ZLINK_BINDING_VERSION`) | as is |
| Each framework | `MAJOR.MINOR.HOTFIX` | `framework/languages/<language>/VERSION` (`ZLINK_FRAMEWORK_VERSION`) | as is |

- **Core has two components.** Any Core change, including a bug fix, increments `MINOR`. Registries that
  require three-component semver publish it with `.0` appended.
- **A binding version identifies its Core.** Its first two components are the exact Core `MAJOR.MINOR`; `N`
  counts fixes to that language binding without a Core change. When Core reaches `0.18`, that binding starts
  again at `0.18.0`. Each language owns its VERSION, so one binding hotfix neither renumbers nor republishes
  another language.
- **A framework uses three components.** Features and contract changes increment `MINOR`; fixes without new
  features and republishes that only update that language's binding pin increment `HOTFIX`. Each language owns
  its VERSION.
- Before 1.0, a `MINOR` increment does not promise compatibility.

## 2. Compatibility

- Binding `X.Y.*` is compatible only with Core `X.Y`.
- A framework language at `A.B` pins the exact binding `X.Y.N` recorded by that language's release. Installing
  the framework installs that binding.
- After 1.0, changes within a `MAJOR` do not break the public Core C API/ABI (`core/include/**`,
  `core/src/libzlink.vers`) or public binding/framework APIs. Breaking changes increment `MAJOR`.

## 3. Release order and tags

| Order | Tag | Workflow | Condition |
| --- | --- | --- | --- |
| 1 | `core/vX.Y.0` | `build.yml` dispatched at the tag | updated `VERSION` and a `core/CHANGELOG.md` section |
| 2 | `<language>/vX.Y.N` | `bindings-release.yml`; .NET uses `release-dotnet.yml` | selected VERSION matches the tag and checked-out Core sources exactly match the Core tag |
| 3 | `framework-<language>/vA.B.C` | `framework-release.yml`; .NET uses `release-dotnet.yml` | selected VERSION matches the tag and that language's binding package is available from its registry |

GitHub may omit workflow events when four or more tags are pushed together (observed during binding 0.17.6
on 2026-09-09). Push tags separately or dispatch the workflow for one language. The complete procedure is in
the [build and release pipeline](./release-pipeline.md).

## 4. Changing a version

- Core: edit only the root `VERSION`.
- Binding: edit the target `bindings/<language>/VERSION` as the source of truth.
- Framework: edit the target `framework/languages/<language>/VERSION` as the source of truth.
- Then run `python3 scripts/local-package/sync-version.py --write` to align manifests, pins, headers, and
  snapshots from the selected source. Run `scripts/local-package/build-wsl.sh --verify-versions` to detect
  omissions and unintended changes to other languages. Only files that cannot read VERSION directly, such as
  JSON, lockfiles, vcpkg, and Conan manifests, remain synchronization targets.

## 5. Plan for 1.0

The 1.0 criteria are:

- A frozen public C API/ABI (`libzlink.vers`) and green release archives/CI for linux-x64, linux-arm64,
  macos-arm64, and windows-x64.
- Frozen public APIs for each language binding and a completed binding-performance campaign on machine B.
- Framework packages aligned with the Core and binding 1.0 line. Release order is Core, the corresponding
  language binding, then the corresponding language framework.
- Updated checksums for the C++ public source archive, vcpkg port, and Conan recipe.

## 6. History

| Date | Decision |
| --- | --- |
| 2026-09-09 | Adopted Core `MAJOR.MINOR`, binding `CORE_MAJOR.CORE_MINOR.N`, and framework `MAJOR.MINOR.HOTFIX`. |
| 2026-09-10 | Split shared binding/framework versions into language-owned VERSION files so a language hotfix and release do not change other language versions. |
