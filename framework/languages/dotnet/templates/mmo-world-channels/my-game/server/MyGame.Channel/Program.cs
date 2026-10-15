using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Channel.Infrastructure.Redis;
using MyGame.Channel.Infrastructure.ZLink;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
builder.Services.AddSingleton<PlayerData>();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf<ZoneSpot>();
        options
            .AddRouteMesh(settings.MeshName)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddEntrySpot<ZoneEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
            )
            .AddSpotFactory<ZoneSpot>(
                GameNames.ZoneType,
                factory =>
                    factory
                        .StableTypeLimit(settings.ZoneCapacity)
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<ZoneRelocation>()
            );
    }
);
await using var host = (IAsyncDisposable)builder.Build();
var runningHost = (IHost)host;
await NodeControl.RunAsync(
    runningHost,
    settings,
    async ct =>
    {
        var spots = runningHost.Services.GetRequiredService<IZLinkSpotManager>();
        foreach (var id in settings.ZoneIds ?? [])
        {
            var created = await spots
                .GetOrCreate(id, GameNames.ZoneType)
                .InMesh(settings.MeshName)
                .Async(ct);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException($"Zone bootstrap rejected: {id}.");
        }
    }
);
