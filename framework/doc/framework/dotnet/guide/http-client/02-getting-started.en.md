[← Table Of Contents](README.en.md)

# 2. Getting Started

## Project Reference

Add the `Zlink.HttpClient` package to the consuming project. Only the framework contract package the
HTTP client uses is installed alongside it — not the whole server runtime.

```xml
<ItemGroup>
  <PackageReference Include="Zlink.HttpClient" Version="0.5.1" />
</ItemGroup>
```

```csharp
using Zlink.HttpClient;
```

## First Request

```csharp
using var client = ZLinkHttpClient.Create("http://127.0.0.1:18080")
    .Build();

var player = await client.Get("/players/7281").Fetch<PlayerProfile>();
Console.WriteLine(player.Name);
```

- Start a builder with `Create(baseUrl)` and build the client with `.Build()`.
- The client is reusable and thread-safe. Typically you create it once and use it for a long time.
- Manage the client's lifetime with `using`.

## One-Line Request

For a one-off request, you can skip `Build()` and call methods directly on the builder.

```csharp
var res = await ZLinkHttpClient.Create("https://game-api.example.internal")
    .Post("/games")
    .Body(new CreateGameReq("ranked-match-0611"))
    .Fetch<CreateGameRes>();
```

If you call it repeatedly, creating the client once and reusing it is better for connection pool
reuse.

## Getting Completion Via Callback

```csharp
client.Get("/leaderboard").Async<Leaderboard>((error, response) =>
{
    if (error is not null)
    {
        Console.Error.WriteLine(error.Message); // Failures are also confirmed in the same callback.
        return;
    }

    Console.WriteLine(response!.Body.TopPlayer); // Use the successful response's typed body.
});
```

The callback suits a client that doesn't use awaitables, or event-loop code. Ordinary code that
needs to keep computing from the completion value `await`s `Async<T>()` instead
([Chapter 7](07-async.en.md)).

[Next: Client Configuration →](03-client-configuration.en.md)
