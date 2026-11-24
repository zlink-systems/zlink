using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Matchmaker;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder(args);
GameHosting.Configure(
    builder,
    settings,
    options =>
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddSpotFactory<PoolSpot>(
                MatchNames.PoolType,
                factory =>
                    factory.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).DisableRelocation()
            )
);
using var host = builder.Build();
await NodeControl.RunAsync(
    host,
    settings,
    async ct =>
    {
        var pool = await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(MatchNames.PoolKey, MatchNames.PoolType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (pool.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Pool creation rejected.");
    }
);
