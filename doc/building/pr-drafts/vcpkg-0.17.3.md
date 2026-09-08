# [zlink] Add new port

## Description

This adds zlink 0.17.3 as a new vcpkg port. The port downloads the immutable
`core/v0.17.3` GitHub tag archive, supports the selected static or dynamic vcpkg
library linkage, builds with TLS through OpenSSL, installs the upstream CMake
configuration, and provides usage guidance for its linkage-specific targets.

## Checklist

- [x] Added `ports/zlink/portfile.cmake`, `vcpkg.json`, and `usage`.
- [x] Pinned the GitHub tag archive with its SHA-512 digest.
- [x] Declared MPL-2.0 and the required OpenSSL/vcpkg CMake dependencies.
- [x] Prepared `versions/z-/zlink.json` and the `versions/baseline.json` entry.
- [x] Checked JSON syntax and the draft submission tree layout.
- [ ] Copied the port into a current `microsoft/vcpkg` checkout.
- [ ] Ran `vcpkg x-add-version zlink` to generate the real `git-tree` value.
- [ ] Ran `vcpkg install zlink` on a supported triplet.
- [ ] Ran the vcpkg CI checks in the submitted pull request.

## Tested platform

- Linux 6.6 (WSL2), x86_64
- Ubuntu 24.04 toolchain, GCC 13.3.0
- Format and archive checksum checks only; no local vcpkg executable was available.

No pull request has been submitted from this draft.
