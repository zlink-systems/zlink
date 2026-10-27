using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Shared;
using MyGame.Town;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf<TownSpot>();
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddEntrySpot<TownEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
            )
            .AddSpotFactory<TownSpot>(
                GameNames.TownType,
                factory =>
                    factory
                        .StableTypeLimit(settings.TownCapacity)
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<TownRelocation>()
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(
    host,
    settings,
    async ct =>
    {
        var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
        foreach (var id in settings.TownIds ?? [])
        {
            var created = await spots
                .GetOrCreate(id, GameNames.TownType)
                .InMesh(GameNames.WorldMesh)
                .Async(ct);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException($"Town bootstrap rejected: {id}.");
        }
    }
);
