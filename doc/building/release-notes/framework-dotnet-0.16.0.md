[English](./framework-dotnet-0.16.0.md) | [한국어](./framework-dotnet-0.16.0.ko.md)

# ZLink .NET Framework 0.16.0 release notes

Framework 0.16.0 uses binding 1.2.0 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- A select-one channel left with no member after eligibility and drain now ends as `Unavailable`, and a request and a one-way send agree. A member dropped because its weight is `0` or because it is draining falls here; the send path and the connection are still there, so it is not `NotFound`. The languages used to answer differently.
- Channel calls used to run through the node-direct mappers, so both a request and a send ended as `NotFound`. Node-direct calls still end as `NotFound`.

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.16.0
```

The release tag is [`framework-dotnet/v0.16.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.16.0).
