using System.Text;
using Tutorial.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

// --8<-- [start:http-client-create]
using var client = ZLinkHttpClient.Create("http://127.0.0.1:5080")
    .Timeout(TimeSpan.FromSeconds(3))
    .Build();
// --8<-- [end:http-client-create]

// --8<-- [start:http-first-request]
var first = await client.Get("/players/p1/profile").Async<PlayerProfile>();
Console.WriteLine($"first request: {first.Body.PlayerId} {first.Body.Nickname}");
// --8<-- [end:http-first-request]

// --8<-- [start:http-request-shaping]
var status = await client.Get("/ops/nodes/game-server-1/status")
    .Header("x-trace-id", "tutorial-1")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<NodeStatus>();
// The admin route has a different base URL, so it uses a separate client.
using var adminClient = ZLinkHttpClient.Create("http://127.0.0.1:5081")
    .BasicAuth("ops", "tutorial-admin")
    .Timeout(TimeSpan.FromSeconds(3))
    .Build();
var weight = await adminClient.Post("/admin/channels/profile/weight")
    .Query("value", "2")
    .Async<WeightResponse>();
Console.WriteLine($"request shaping: status {status.Status} weight {weight.Body.Weight}");
// --8<-- [end:http-request-shaping]

// The room id created here is reused by the room, response-kind, compression,
// download, and upload examples below.
// --8<-- [start:http-json-body]
var player = await client.Post("/players/p2")
    .Body(new CreatePlayer("rookie"))
    .Async<string>();
var room = await client.Post("/rooms")
    .Body(new OpenRoom("tutorial-room"))
    .Async<string>();
var chat = await client.Post($"/rooms/{room.Body}/chat")
    .Body(new PostChat("p2", "hello"))
    .AsyncRaw();
Console.WriteLine($"json body: player {player.Status} room {room.Body} chat {chat.Status}");
// --8<-- [end:http-json-body]

// --8<-- [start:http-response-kinds]
var typed = await client.Get("/players/p2").Async<PlayerInfo>();
var raw = await client.Get("/players/p2").AsyncRaw();
var fetched = await client.Get("/players/p2").Fetch<PlayerInfo>();
var contentType = raw.Headers.TryGetValue("content-type", out var rawContentType)
    ? rawContentType
    : "";
Console.WriteLine($"response kinds: typed {typed.Status} raw {contentType} fetch {fetched.Nickname}");
// --8<-- [end:http-response-kinds]

// --8<-- [start:http-compressed-response]
using var compressedClient = ZLinkHttpClient.Create("http://127.0.0.1:5080")
    .Compression()
    .Build();
var compressed = await compressedClient.Get($"/rooms/{room.Body}").Async<RoomState>();
var encodingRemoved = !compressed.Headers.Keys.Any(
    key => key.Equals("content-encoding", StringComparison.OrdinalIgnoreCase));
Console.WriteLine($"compressed response: {compressed.Status} encoding-removed {encodingRemoved}");
// --8<-- [end:http-compressed-response]

// --8<-- [start:http-redirect]
using var redirectClient = ZLinkHttpClient.Create("http://127.0.0.1:5080")
    .FollowRedirects()
    .Build();
// The redirect target is an actor route, so create p1 before reading its legacy URL.
await client.Post("/players/p1")
    .Body(new CreatePlayer("rookie"))
    .AsyncRaw();
var redirected = await redirectClient.Get("/player/p1").Async<PlayerInfo>();
Console.WriteLine($"redirect: {redirected.Status} {redirected.Body.PlayerId}");
// --8<-- [end:http-redirect]

// --8<-- [start:http-basic-auth]
using var unauthenticatedAdmin = ZLinkHttpClient.Create("http://127.0.0.1:5081")
    .Build();
var withoutAuth = await unauthenticatedAdmin.Post("/admin/channels/profile/weight")
    .Query("value", "2")
    .AsyncRaw();
using var authenticatedAdmin = ZLinkHttpClient.Create("http://127.0.0.1:5081")
    .BasicAuth("ops", "tutorial-admin")
    .Build();
var withAuth = await authenticatedAdmin.Post("/admin/channels/profile/weight")
    .Query("value", "2")
    .AsyncRaw();
Console.WriteLine($"basic auth: without {withoutAuth.Status} with {withAuth.Status}");
// --8<-- [end:http-basic-auth]

// --8<-- [start:http-download-stream]
var downloadChunks = 0;
var downloadBytes = 0;
var downloaded = await client.Get($"/rooms/{room.Body}/export")
    .DownloadAsync(chunk =>
    {
        downloadChunks++;
        downloadBytes += chunk.Length;
    });
Console.WriteLine($"download stream: chunks {downloadChunks} bytes {downloadBytes}");
// --8<-- [end:http-download-stream]

// --8<-- [start:http-upload-stream]
var uploadChunks = new Queue<byte[]>(new[]
{
    Encoding.UTF8.GetBytes("{\"playerId\":\"p2\",\"text\":\"one\"}\n"),
    Encoding.UTF8.GetBytes("{\"playerId\":\"p2\",\"text\":\"two\"}\n"),
    Encoding.UTF8.GetBytes("{\"playerId\":\"p2\",\"text\":\"three\"}\n")
});
var imported = await client.Post($"/rooms/{room.Body}/import")
    .BodyStream(
        () => uploadChunks.Count > 0 ? uploadChunks.Dequeue() : null,
        "application/x-ndjson")
    .Async<ImportResponse>();
Console.WriteLine($"upload stream: imported {imported.Body.Imported}");
// --8<-- [end:http-upload-stream]

// --8<-- [start:http-error-kinds]
ZLinkFrameworkException badRequest;
try
{
    await client.Post("/players/p3").Async<string>();
    throw new InvalidOperationException("bad request unexpectedly succeeded");
}
catch (ZLinkFrameworkException exception)
{
    badRequest = exception;
}

ZLinkFrameworkException connectionRefused;
using (var closedPortClient = ZLinkHttpClient.Create("http://127.0.0.1:5980").Build())
{
    try
    {
        await closedPortClient.Get("/players/p1").AsyncRaw();
        throw new InvalidOperationException("closed port unexpectedly succeeded");
    }
    catch (ZLinkFrameworkException exception)
    {
        connectionRefused = exception;
    }
}
Console.WriteLine($"error kinds: bad request {badRequest.Kind} connection refused {connectionRefused.Kind}");
// --8<-- [end:http-error-kinds]

public sealed record WeightResponse(string Channel, int Weight);

public sealed record ImportResponse(int Imported);
