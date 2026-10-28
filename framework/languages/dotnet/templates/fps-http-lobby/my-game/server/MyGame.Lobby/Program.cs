using MyGame.Lobby.Application;
using MyGame.Lobby.Infrastructure.ZLink;
using MyGame.Shared;

var settings = NodeSettings.Load(args);
ArgumentException.ThrowIfNullOrWhiteSpace(settings.HttpEndpoint);
ArgumentException.ThrowIfNullOrWhiteSpace(settings.SessionEndpoint);
var builder = WebApplication.CreateBuilder();
builder.WebHost.UseUrls(settings.HttpEndpoint);
GameHosting.Configure(
    builder,
    settings,
    options =>
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Client()
);
builder.Services.AddSingleton<IRoomProvisioner, RoomProvisioner>();
builder.Services.AddSingleton<Matchmaker>();
await using var app = builder.Build();
app.MapPost(
    LobbyNames.MatchPath,
    async (MatchRequest request, Matchmaker matchmaker, CancellationToken ct) =>
    {
        try
        {
            return Results.Ok(await matchmaker.MatchAsync(request, ct));
        }
        catch (ArgumentException error)
        {
            return Results.BadRequest(new { error = error.Message });
        }
    }
);
await NodeControl.RunAsync(app, settings);
