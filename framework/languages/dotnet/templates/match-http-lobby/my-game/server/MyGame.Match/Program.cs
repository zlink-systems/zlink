using Microsoft.Extensions.Hosting;
using MyGame.Match;
using MyGame.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(builder, settings, options =>
{
    options.AddHandlersFromAssemblyOf(typeof(RoomSpot));
    options.AddRouteMesh(GameNames.WorldMesh).SetRoutingIdPrefix(settings.Id).Listen(settings.MeshEndpoint)
        .Objects().Server().AddEntrySpot<MatchEntry>()
        .AddActorFactory<PlayerActor, PlayerFactory>(GameNames.PlayerType, factory => factory.PreserveStateWith<PlayerRelocation>())
        .AddSpotFactory<RoomSpot>(GameNames.RoomType, factory => factory
            .StableTypeLimit(settings.RoomCapacity)
            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
            .PreserveStateWith<RoomRelocation>());
});
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
