using Microsoft.Extensions.Hosting;
using MyGame.Instance;
using MyGame.Shared;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder(args);
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf<DungeonSpot>();
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddSpotFactory<DungeonSpot>(
                GameNames.DungeonType,
                factory => factory.RecreateOnRelocation()
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
