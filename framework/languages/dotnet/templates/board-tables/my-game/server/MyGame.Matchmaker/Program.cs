using Microsoft.Extensions.Hosting;
using MyGame.Matchmaker;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf(typeof(PoolSpot));
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(
                settings.MeshEndpoint
                    ?? throw new InvalidOperationException("MeshEndpoint is required.")
            )
            .Objects()
            .Server()
            .AddInstanceSpotFactory<PoolSpot>(
                MatchNames.PoolType,
                factory => factory.DisableRelocation()
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
