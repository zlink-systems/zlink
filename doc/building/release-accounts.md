[English](./release-accounts.md) | [한국어](./release-accounts.ko.md)

# Official Release Accounts/Secrets Checklist

This document lists the accounts and GitHub Actions Secrets required for official releases.

## 1. GitHub Repository Permissions

- `Contents: write` permission (for creating Releases)
- Tag push permission (`core/v*`, `node/v*`, `python/v*`, `java/v*`, `dotnet/v*`, `cpp/v*`)

## 2. Node (npm)

- Required account: npm organization/package publish permission
- Required secrets:
  - `NPM_TOKEN`: Automation token with npm publish capability

## 3. Python (PyPI)

- Required account: PyPI project owner/maintainer
- Required secrets:
  - `PYPI_API_TOKEN`: PyPI API token (for `__token__` upload)

## 4. Java (Maven Repository)

The current workflow is configured to publish to the GitHub Packages Maven registry by default.

- Required account: GitHub account + repository `packages: write` permission
- Required secrets: None (uses the default `GITHUB_TOKEN`)

To publish to an external Maven repository (Nexus/Artifactory/Sonatype, etc.), configure the following additional secrets:

- `MAVEN_REPOSITORY_URL`
- `MAVEN_REPOSITORY_USERNAME`
- `MAVEN_REPOSITORY_PASSWORD`

## 5. .NET (NuGet)

- Required account: NuGet.org (or internal NuGet server) publish permission
- Required secrets:
  - `NUGET_SOURCE_URL` (e.g., `https://api.nuget.org/v3/index.json`)
  - `NUGET_API_KEY`

## 6. Conan (Core)

For open-source official distribution via ConanCenter, the process is recipe PR-based rather than upload token-based.

The following secrets are only needed when uploading to an internal/personal Conan remote:

- `CONAN_REMOTE_URL`
- `CONAN_LOGIN_USERNAME`
- `CONAN_PASSWORD`

## 7. vcpkg

- Overlay port distribution (current setup): No separate account required
- Official vcpkg ports inclusion: Requires GitHub account + PR approval

## 8. Workflow Files

- Core release: `.github/workflows/build.yml`
- Core Conan release: `.github/workflows/core-conan-release.yml`
- Bindings release/registry publish: `.github/workflows/bindings-release.yml`
