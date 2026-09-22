using EngineLobby;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;

if (args is ["probe", var probeEndpoint])
{
    await EngineLobbyProbe.RunAsync(probeEndpoint);
    return;
}

if (args is not ["server", var settingsPath])
    throw new InvalidOperationException(
        "Usage: EngineLobby server <settings.json> | probe <ws-endpoint>"
    );

var settings = EngineLobbySettings.Load(settingsPath);
var builder = WebApplication.CreateBuilder([]);
builder.WebHost.UseUrls(settings.HttpEndpoint);

builder.Services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(redis =>
        {
            redis.ConnectionString = settings.RedisEndpoint;
            redis.KeyPrefix = settings.RedisKeyPrefix;
        })
    );

    var objects = options
        .AddRouteMesh("engine-lobby")
        .Listen(settings.MeshEndpoint)
        .Objects()
        .Server();

    objects.AddEntrySpot<LobbySpot>();
    objects.AddActorFactory<ParticipantActor, ParticipantActorFactory>(
        "participant",
        factory => factory.DisableRelocation()
    );

    options
        .AddStreamNode("engine-lobby-stream")
        .Bind(settings.StreamEndpoint)
        .EnableActorDispatch()
        .AddSession<LobbySession>();
});

var app = builder.Build();
app.MapGet("/ready", () => Results.Ok(new { ready = true }));
await app.RunAsync();
