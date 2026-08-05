[English](./packaging.md) | [한국어](./packaging.ko.md)

# Packaging and Release (Core / Bindings Separation)

## 1. Goals

- Release Core (`libzlink`) and bindings (Node/Python/Java/.NET/C++) as **separate processes**.
- After the Core release is complete, bindings go through language-specific modifications/verification and are published with **individual tags**.
- Maintain both Conan and vcpkg (overlay port).

## 2. Tag Conventions

- Core: `core/vX.Y.Z`
- Node: `node/vA.B.C`
- Python: `python/vA.B.C`
- Java: `java/vA.B.C`
- .NET: `dotnet/vA.B.C`
- C++ binding: `cpp/vA.B.C`

Core version (`VERSION`) and binding versions are managed independently.

## 3. GitHub Actions

### Core Release

- Workflow: `.github/workflows/build.yml`
- Triggers: `core/v*` tag, `workflow_dispatch`
- Artifacts:
  - Platform-specific core native archives
  - Source tarball
  - Checksums
- Conan release workflow: `.github/workflows/core-conan-release.yml`

### Bindings Release

- Workflow: `.github/workflows/bindings-release.yml`
- Triggers:
  - Language-specific tags (`node/v*`, `python/v*`, `java/v*`, `dotnet/v*`, `cpp/v*`)
  - `workflow_dispatch` (specify target/version)
- Actions:
  - Validate language-specific version files
  - Run language-specific tests/packaging
  - Create GitHub Release if needed
  - Publish to npm/PyPI/NuGet/Maven if needed

Required accounts/secrets: [release-accounts.md](./release-accounts.md)

## 4. Conan

Managed based on the Core source tarball.

Official distribution (ConanCenter) is recipe PR-based; uploading to an internal remote is optional.

### Related Files

| File | Path |
|------|------|
| conanfile | `core/packaging/conan/conanfile.py` |
| conandata | `core/packaging/conan/conandata.yml` |
| README | `core/packaging/conan/README.md` |

### Update Procedure

1. Release `core/vX.Y.Z` tag
2. Verify tarball URL/sha256
3. Update `conandata.yml`
4. Run `conan create . --version X.Y.Z`

## 5. vcpkg (overlay port)

Official distribution (vcpkg ports) is PR-based; the overlay port is maintained for development/verification.

### Related Files

| File | Path |
|------|------|
| portfile | `vcpkg/ports/zlink/portfile.cmake` |
| vcpkg.json | `vcpkg/ports/zlink/vcpkg.json` |
| README | `vcpkg/README.md` |

### Update Procedure

1. Release `core/vX.Y.Z` tag
2. Update `version-string` in `vcpkg/ports/zlink/vcpkg.json`
3. Verify overlay installation:

```bash
vcpkg install zlink --overlay-ports=./vcpkg/ports
```

## 6. Bindings Release Operation Example

1. Release Core `core/v0.10.0`
2. Modify/verify Node binding
3. Publish Node only with `node/v1.3.0` tag
4. Publish Python separately with `python/v0.10.2` when ready

## 7. Checklist

- [ ] Update Core `VERSION`
- [ ] Release Core tag (`core/vX.Y.Z`)
- [ ] Update Conan metadata
- [ ] Update vcpkg overlay version
- [ ] Update version files for each binding
- [ ] Release each binding individually with its own tag
