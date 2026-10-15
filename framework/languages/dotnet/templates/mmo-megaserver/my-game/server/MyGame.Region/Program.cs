using Microsoft.Extensions.Hosting;
using MyGame.Region;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf<RegionSpot>();
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddEntrySpot<RegionEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
            )
            .AddSpotFactory<RegionSpot>(
                GameNames.RegionType,
                factory =>
                    factory
                        .StableTypeLimit(settings.RegionLimit)
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<RegionRelocation>()
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
