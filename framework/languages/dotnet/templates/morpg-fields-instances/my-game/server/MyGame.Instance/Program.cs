using Microsoft.Extensions.Hosting;
using MyGame.Instance;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(builder, settings, options =>
{
    options.AddHandlersFromAssemblyOf<DungeonSpot>();
    options.AddRouteMesh(GameNames.WorldMesh).SetRoutingIdPrefix(settings.Id).Listen(settings.MeshEndpoint)
        .Objects().Server()
        .AddActorFactory<PlayerActor, PlayerFactory>(GameNames.PlayerType,
            factory => factory.PreserveStateWith<PlayerRelocation>())
        .AddSpotFactory<DungeonSpot>(GameNames.DungeonType,
            factory => factory.StableTypeLimit(settings.DungeonCapacity).ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                .PreserveStateWith<DungeonRelocation>());
});
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
