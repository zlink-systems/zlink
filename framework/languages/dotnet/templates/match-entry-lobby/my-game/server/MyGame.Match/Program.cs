using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Match;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder(args);
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf(typeof(RoomSpot));
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddEntrySpot<MatchEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                f => f.PreserveStateWith<PlayerRelocation>()
            )
            .AddSpotFactory<MatchmakingSpot>(
                GameNames.MatchmakingType,
                f =>
                    f.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<MatchmakingRelocation>()
            )
            .AddSpotFactory<RoomSpot>(
                GameNames.RoomType,
                f =>
                    f.StableTypeLimit(settings.RoomCapacity)
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<RoomRelocation>()
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(
    host,
    settings,
    async ct =>
    {
        if (!settings.BootstrapMatchmaking)
            return;
        var result = await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(GameNames.MatchmakingId, GameNames.MatchmakingType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (result.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Matchmaking bootstrap rejected.");
    }
);
