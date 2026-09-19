# .NET Quickstart — from Install to a First Request

!!! info "What you get from this chapter"

    You can install the packages and run a minimal project where two processes call each other.

The project lives at
[`framework/languages/dotnet/quickstart/`](../../../languages/dotnet/quickstart/). The code
blocks below are read from those files when the site is built. Without a location store, two
processes name each other's endpoint directly and exchange one request/reply.

## 0. Downloading the tutorial

This chapter builds the smallest project from scratch. **To run the finished tutorial instead**,
one archive is all you need — there is no reason to clone the whole repository.

!!! tip "Download the tutorial"

    [:material-download: **zlink-tutorial-dotnet.zip**](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-tutorial-dotnet.zip){ .md-button .md-button--primary }


The address does not depend on the platform: Windows and WSL fetch the same file. Unpacking it
leaves the project under `zlink-tutorial-dotnet/`, with the package versions of the release you
downloaded.

For the current main, take just that directory out of the repository.

```bash
git clone --filter=blob:none --sparse https://github.com/zlink-systems/zlink.git
cd zlink
git sparse-checkout set framework/languages/dotnet/tutorial
```

## 1. Installation

- .NET SDK 8.0 or later (`net8.0`)
- Access to nuget.org

Get it from NuGet. The minimal combination needed to build one server is the following.

```bash
# The contract and runtime. It includes the core messaging engine (Zlink) transitively
dotnet add package Zlink.Framework
# DI/hosted service registration (AddZLinkFramework)
dotnet add package Zlink.Framework.AspNetCore
```

**Do not list `Zlink` (the binding) yourself.** `Zlink.Framework` declares the version it depends on.

```xml title="Directory.Packages.props"
--8<-- "framework/languages/dotnet/quickstart/Directory.Packages.props"
```

```xml title="nuget.config"
--8<-- "framework/languages/dotnet/quickstart/nuget.config"
```

Packages to add when you need them:

| Package | When to add it |
| --- | --- |
| `Zlink.Framework.Locations.Redis` | When using the Redis location store for auto-connect ([Location](guide/server/25-location.en.md)) |
| `Zlink.Framework.Codecs.Protobuf` · `.MessagePack` | To use instead of the default JSON codec ([Handlers and Message Processing](guide/server/31-handler-dispatch.en.md#3-codecs--turning-a-payload-into-bytes)) |
| `Zlink.Stream.Connector` | When building an external client (a game client, mobile) ([STREAM](guide/server/23-stream.en.md)) |
| `Zlink.HttpClient` | When the server calls out over HTTP ([HTTP Client guide](guide/http-client/README.en.md)) |

The license differs by layer — core/binding is
MPL-2.0, framework is FSL-1.1-ALv2, and `Zlink.HttpClient` is Apache-2.0. There is no cost to
building and selling a service
([Where ZLink Applies](guide/server/17-alternative.en.md#8-license--the-cost-of-using-it)).

## 2. Shared contract

```csharp title="Shared/Contracts.cs"
--8<-- "framework/languages/dotnet/quickstart/Shared/Contracts.cs"
```

## 3. The handling side

A handler implementing `IZLinkRequestHandler<,>` is registered directly on
`Channel(...).Server().AddRequestHandler<...>()`. That registration is what decides which channel
exposes it.

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

## 6. What to check when the first run fails

| Symptom | What to check |
| --- | --- |
| A package is not found | Check that the package names in section 1 were copied exactly. The binding version is not pinned separately |
| Startup fails | Check that both processes name the same mesh, and that the listen endpoint does not collide with another process |
| A call ends with no target | Check that the receiving side registered that channel name in the server role, and that the two processes are connected as peers |
| No answer arrives | Check that the caller used `request`. A `send` receives no answer |

## 7. What to carry over

| File | Content |
|---|---|
| `Directory.Packages.props` | The two `PackageVersion` entries. The binding stays transitive |
| `Shared/Contracts.cs` | `record`-based message definitions |
| `Server/Program.cs` | `AddRouteMesh` → `Listen` → `Channel(...).Server().AddRequestHandler<...>()` |
| `Client/Program.cs` | `Channel(...).Client()`, `PeerConnections.Connect(...)`, `RequestToChannel(...).Async<T>()` |

## 8. What to read next

These two processes connect by writing each other's endpoint directly. Keeping the calling code
unchanged while servers are added or restarted at another address needs automatic connection, and
that is covered by [Location](guide/server/25-location.en.md).

- To go over the concepts first — [Core Concepts](guide/server/03-concepts.en.md)
- The path that calls by name — [Channel Messaging](guide/server/20-channel-messaging.en.md)
- State objects called by id — [Spot](guide/server/21-spot.en.md) · [Actor](guide/server/22-actor.en.md)
- To see a complete business flow — [Picking a Sample](guide/server/14-samples.en.md)
