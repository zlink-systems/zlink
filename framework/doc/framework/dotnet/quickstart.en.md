# .NET Quickstart — from an empty project to a first request

> **Contract owner for this chapter** — none. The formal API contract is in the
> [.NET spec](../common/spec/server/languages/dotnet/README.en.md).

The project lives at
[`framework/languages/dotnet/quickstart/`](../../../languages/dotnet/quickstart/). The code
blocks below are read from those files when the site is built.

Without a location store, two processes name each other's endpoint directly and exchange one
request/reply. The next step is
[Installation and first run](guide/server/02-getting-started.en.md).

## Prerequisites

- .NET SDK 8.0 or later (`net8.0`)
- Access to nuget.org

## 1. Package versions

`Zlink` (the binding) is not listed. `Zlink.Framework` declares the version it depends on.

```xml title="Directory.Packages.props"
--8<-- "framework/languages/dotnet/quickstart/Directory.Packages.props"
```

```xml title="nuget.config"
--8<-- "framework/languages/dotnet/quickstart/nuget.config"
```

## 2. Shared contract

```csharp title="Shared/Contracts.cs"
--8<-- "framework/languages/dotnet/quickstart/Shared/Contracts.cs"
```

## 3. The handling side

`AddHandlersFromAssemblyOf` only discovers handler types. Which channel exposes one is a
separate registration on `Channel(...).Server()`.

```csharp title="Server/Program.cs"
--8<-- "framework/languages/dotnet/quickstart/Server/Program.cs"
```

```xml title="Server/Server.csproj"
--8<-- "framework/languages/dotnet/quickstart/Server/Server.csproj"
```

## 4. The calling side

Both processes run on the same host, so each names its own HTTP port. Without that they
collide on Kestrel's default port.

```csharp title="Client/Program.cs"
--8<-- "framework/languages/dotnet/quickstart/Client/Program.cs"
```

## 5. Run

```bash
cd framework/languages/dotnet/quickstart
dotnet build

# Two terminals. Start the server first.
dotnet run --project Server/Server.csproj
dotnet run --project Client/Client.csproj

curl http://127.0.0.1:5080/hello/world
```

The response is `"hello, world"` with status 200.

## What to carry over

| File | Content |
|---|---|
| `Directory.Packages.props` | The two `PackageVersion` entries. The binding stays transitive |
| `Shared/Contracts.cs` | `record`-based message definitions |
| `Server/Program.cs` | `AddRouteMesh` → `Listen` → `Channel(...).Server().AddRequestHandler<...>()` |
| `Client/Program.cs` | `Channel(...).Client()`, `PeerConnections.Connect(...)`, `RequestToChannel(...).Async<T>()` |

Replacing the manual `PeerConnections.Connect` with a location store is covered by
[10. Location](guide/server/10-location.en.md).
