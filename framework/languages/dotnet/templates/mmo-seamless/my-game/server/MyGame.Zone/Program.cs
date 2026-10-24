using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Shared;
using MyGame.Zone;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder(args);
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf<ZoneSpot>();
        var mesh = options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint);
        mesh.Objects()
            .Server()
            .AddEntrySpot<WorldEntry>()
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
        mesh.Channel(GameNames.BorderChannel).Server();
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(
    host,
    settings,
    async cancellationToken =>
    {
        var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
        foreach (var zoneId in settings.ZoneIds ?? [])
        {
            var created = await spots
                .GetOrCreate(zoneId, GameNames.ZoneType)
                .InMesh(GameNames.WorldMesh)
                .Async(cancellationToken);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException($"Zone bootstrap rejected: {zoneId}.");
        }
    }
);
