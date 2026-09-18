[English](./framework-dotnet-0.17.0.md) | [한국어](./framework-dotnet-0.17.0.ko.md)

# ZLink .NET Framework 0.17.0 release notes

Framework 0.17.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- Sending through a bound session from an Actor with no binding now ends the same way in all five languages. With no valid binding the call ends as `InvalidOperation`, and that failure surfaces at the **call's terminal** like every other call failure, rather than being thrown where the call is built.
- .NET already had this shape, so its behaviour is unchanged.

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.17.0
```

The release tag is [`framework-dotnet/v0.17.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.17.0).
