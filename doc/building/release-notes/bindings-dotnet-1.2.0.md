[English](./bindings-dotnet-1.2.0.md) | [한국어](./bindings-dotnet-1.2.0.ko.md)

# ZLink .NET binding 1.2.0 release notes

This release runs on Core 1.2.0. The 1.1.0 binding assumes the Core 1.1.0 ABI and cannot be used with Core 1.2.0; this release closes that gap.

## Changes

- Binds to the Core 1.2.0 native library. The native library and provenance in the package are the Core 1.2.0 release assets.
- `PublishSubmitOperation.Submit()` passes the caller's flags through. The default stays immediate (DontWait); an explicit `.Flags(SendFlags.None)` waits for local admission up to `SendTimeout` (#456).
- `CommonSocketOptions.SendTimeout` now maps to `SNDTIMEO` (#456).
## Verification

- The binding test suite passes on the Core 1.2.0 package.

The release tag is `dotnet/v1.2.0`.
