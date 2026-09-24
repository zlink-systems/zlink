[English](./bindings-java-1.6.0.md) | [한국어](./bindings-java-1.6.0.ko.md)

# ZLink Java binding 1.6.0 release notes

This release aligns the Java binding package with Core 1.6.0.

## Changes

- The Java binding package version is 1.6.0 for the Core 1.6.0 release.
- Core 1.6.0 allows ROUTER to publish a reply token after a complete REQUEST is received, even if the source pipe disconnects. The public C API and ABI are unchanged ([#1051](https://github.com/zlink-systems/zlink/issues/1051)).
- This release changes no Java binding behavior; it aligns the binding version with Core 1.6.0.

## Verification

- The release preflight verifies the Java binding version, release notes, and Maven package metadata.

The release tag is `java/v1.6.0`.
