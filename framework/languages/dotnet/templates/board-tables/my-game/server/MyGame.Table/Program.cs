using Microsoft.Extensions.Hosting;
using MyGame.Shared;
using MyGame.Table;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

var settings = NodeSettings.Load(args);
var builder = Host.CreateApplicationBuilder(args);
GameHosting.Configure(
    builder,
    settings,
    options =>
    {
        options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint)
            .Objects()
            .Server()
            .AddEntrySpot<TableEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
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
