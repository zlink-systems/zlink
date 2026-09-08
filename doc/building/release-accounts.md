[English](./release-accounts.md) | [한국어](./release-accounts.ko.md)

# Public Release Accounts and Secrets

This document records the confirmed accounts, namespaces, and GitHub Actions secret names used for
public zlink releases. The Python, Go, and Rust bindings are outside this release scope.

## Public Channels

| Channel | Account or namespace | Distribution method |
| --- | --- | --- |
| GitHub | `zlink-systems` organization, `zlink-systems/zlink` repository | Language tags and GitHub Release assets |
| Maven Central | Verified `systems.zlink` namespace | Sonatype Central Portal bundle upload |
| nuget.org | Personal account `zlink` | Trusted Publishing (OIDC), no API key |
| npm | Personal account `zlink-systems`, `@zlink-systems` scope, 2FA | First release published manually by the user; then connect Trusted Publishing with provenance |
| ConanCenter | GitHub account `zlink-systems` | Recipe pull request |
| vcpkg | GitHub account `zlink-systems` | Ports pull request |

The nuget.org Trusted Publishing policy is fixed to these values:

| Field | Value |
| --- | --- |
| Policy | `zlink-dotnet-release` |
| Repository owner | `zlink-systems` |
| Repository | `zlink` |
| Workflow filename | `release-dotnet.yml` |
| Environment | none |
| Package glob | `Zlink*` |

Both binding and Framework .NET packages must therefore be pushed from
`.github/workflows/release-dotnet.yml`. An OIDC token obtained by another workflow does not match
this policy.

## GitHub Actions Secrets

Public release workflows reference only these four repository secrets. Record their names, never
their values, in documentation and logs.

| Secret | Purpose |
| --- | --- |
| `MAVEN_CENTRAL_USERNAME` | Sonatype Central Portal token username |
| `MAVEN_CENTRAL_PASSWORD` | Sonatype Central Portal token password |
| `SIGNING_KEY` | Base64-encoded armored GPG private key |
| `SIGNING_PASSPHRASE` | GPG private-key passphrase |

Use the existing signing key. Do not generate a new key or token.

## Unused Secrets and Internal Remotes

| Name | Handling |
| --- | --- |
| `NPM_TOKEN` | Not used. The first npm release is manual; subsequent releases use OIDC |
| `NUGET_API_KEY` | Not used. Use only the short-lived token returned by `NuGet/login@v1` |
| `PYPI_API_TOKEN` | Not used because Python is outside this release scope |
| `MAVEN_REPOSITORY_URL` | Internal Maven remote only; not used for Maven Central |
| `MAVEN_REPOSITORY_USERNAME` | Internal Maven remote only |
| `MAVEN_REPOSITORY_PASSWORD` | Internal Maven remote only |

ConanCenter and vcpkg public distribution use no registry login secret. Both are proposed through
GitHub pull requests; this preparation work stops at drafts.

## Public Release Order

1. Verify the `core/v0.17.3` GitHub Release and assets.
2. Publish the C++, Node.js, Java, and .NET bindings at 0.17.3.
3. Verify binding installation and checksums from each public channel.
4. Publish Framework 0.10.0 for C++, Node.js, Java, and .NET. Push .NET packages from
   `release-dotnet.yml`.

The relevant workflow drafts are `.github/workflows/bindings-release.yml`,
`.github/workflows/release-dotnet.yml`, and `.github/workflows/framework-release.yml`.
