[English](./framework-cpp-0.21.0.md) | [한국어](./framework-cpp-0.21.0.ko.md)

# ZLink C++ Framework 0.21.0 Release Notes

Framework 0.21.0 uses binding 1.2.1 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- None. The public contract is the same as 0.20.0.

## Common Changes

- Tutorial, quickstart and sample sources follow formatter rule 4 (the right-hand side of an assignment starts on the `=` line; chains continue below it). (#847)
- The quickstart README is rewritten as the standard ko/en procedure and joins the formatter scope. (#838)
- The runtime and the public contract are unchanged from 0.20.0. This release cleans up the tutorial, samples, quickstart and the repository tooling.
- Quickstart, tutorial and samples are obtained from the per-language examples repository (`zlink-<lang>-examples`); the mirror workflow preserves executable bits (#831) and the README starts with an English | 한국어 switch.
- The tutorial CI C++ job keeps the vcpkg binary cache in the Actions cache. (#852)
- google-java-format 1.27.0 runs the Java and Kotlin format check under JDK 25. (#798)
- The unused v11 public-contract trace generator and inventories are removed. (#747)

## Install

Download `zlink-framework-cpp-0.21.0.tar.gz` from the [`framework-cpp/v0.21.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.21.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. A vcpkg overlay port and Conan recipe are also available. C++ binding 1.2.1 and Core 1.2.0 are required.

The release tag is [`framework-cpp/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.21.0).
