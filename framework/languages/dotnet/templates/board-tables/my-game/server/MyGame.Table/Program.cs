using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Shared;
using MyGame.Table;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder();
builder.Services.AddSingleton<IPlayerRecords, ChannelPlayerRecords>();
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options.AddHandlersFromAssemblyOf(typeof(RoomProvisioner));
        options.AddClientServerChannel(PlayerRecords.Channel).Client();
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(
                settings.MeshEndpoint
                    ?? throw new InvalidOperationException("MeshEndpoint is required.")
            )
            .Objects()
            .Server()
            .AddEntrySpot<TableEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
            )
            .AddInstanceSpotFactory<RoomProvisioner>(
                MatchNames.RoomProvisionerType,
                factory => factory.RecreateOnRelocation()
            )
            .AddSpotFactory<TableSpot>(
                GameNames.TableType,
                factory =>
                {
                    if (settings.TableCapacity > 0)
                        factory.StableTypeLimit(settings.TableCapacity);
                    factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<TableRelocation>();
                }
            );
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(host, settings);
