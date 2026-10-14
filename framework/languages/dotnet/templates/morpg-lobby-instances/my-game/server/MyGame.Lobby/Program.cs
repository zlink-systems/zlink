using Microsoft.Extensions.Hosting;
using MyGame.Shared;
using MyGame.Lobby;
using Zlink.Framework.Contracts.Configuration;
var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
GameHosting.Configure(builder, settings, options =>
{
    options.AddHandlersFromAssemblyOf<PartySpot>();
    options.AddRouteMesh(GameNames.WorldMesh).SetRoutingIdPrefix(settings.Id)
        .Listen(settings.MeshEndpoint).Objects().Server()
        .AddEntrySpot<LobbyEntry>()
        .AddActorFactory<PlayerActor, PlayerFactory>(GameNames.PlayerType,
            factory => factory.PreserveStateWith<PlayerRelocation>())
        .AddSpotFactory<PartySpot>(GameNames.PartyType,
            factory => factory.StableTypeLimit(settings.PartyCapacity)
                .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                .PreserveStateWith<PartyRelocation>());
});
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
