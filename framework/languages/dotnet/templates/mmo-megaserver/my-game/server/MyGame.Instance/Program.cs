using Zlink.Framework.Contracts.Configuration;
using Microsoft.Extensions.Hosting;
using MyGame.Instance;
using MyGame.Shared;
using Zlink.Framework.Contracts.Spots;
var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(builder, settings, options =>
{
    options.AddHandlersFromAssemblyOf<DungeonSpot>();
    options.AddRouteMesh(GameNames.WorldMesh).SetRoutingIdPrefix(settings.Id).Listen(settings.MeshEndpoint)
        .Objects().Server().AddSpotFactory<DungeonSpot>(GameNames.DungeonType,
            factory => factory.RecreateOnRelocation().ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide));
});
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
