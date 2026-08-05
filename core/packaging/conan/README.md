# Conan Packaging (zlink)

## Overview
This recipe pulls the release source tarball from GitHub.
Update `conandata.yml` when a new release tag is created.

## Update Steps
1) Create release tag and GitHub release.
2) Download the source tarball (or use `git archive`).
3) Update `conandata.yml` with URL + sha256.
4) Run `conan create . --version <VERSION>` from this folder.

## Notes
- Version should match `VERSION` in repo (e.g., 10.0.0 or 10.0.0-rc.1).
- URL is expected to be
  `https://github.com/kairos-code-dev/zlink/archive/refs/tags/core/v<VERSION>.tar.gz`.
- The release workflow refuses a `conandata.yml` entry without a `sha256`
  digest, so fill the digest immediately after pushing the tag.
