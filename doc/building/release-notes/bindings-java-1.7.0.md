[English](./bindings-java-1.7.0.md) | [한국어](./bindings-java-1.7.0.ko.md)

# ZLink Java binding 1.7.0 release notes

This release aligns the Java binding package with Core 1.7.0.

## Changes

- The Java binding package version is 1.7.0 and uses Core 1.7.0.
- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- This release changes no Java binding behavior.

## Verification

- The release preflight verifies the Java binding version, release notes, and Maven package metadata.

The release tag is `java/v1.7.0`.
