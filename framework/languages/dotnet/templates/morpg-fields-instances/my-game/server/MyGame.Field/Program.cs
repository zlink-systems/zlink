using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using MyGame.Field;
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
        options.AddHandlersFromAssemblyOf<FieldSpot>();
        options.AddHandlersFromAssemblyOf<WorldEntry>();
        var mesh = options
            .AddRouteMesh(GameNames.WorldMesh)
            .SetRoutingIdPrefix(settings.Id)
            .Listen(settings.MeshEndpoint);
        mesh.Objects()
            .Server()
            .AddEntrySpot<WorldEntry>()
            .AddActorFactory<PlayerActor, PlayerFactory>(
                GameNames.PlayerType,
                factory => factory.PreserveStateWith<PlayerRelocation>()
            )
            .AddSpotFactory<FieldSpot>(
                GameNames.FieldType,
                factory =>
                    factory
                        .StableTypeLimit(settings.FieldCapacity)
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<FieldRelocation>()
            );
        mesh.Channel(GameNames.BorderChannel).Server();
    }
);
using var host = builder.Build();
await NodeControl.RunAsync(
    host,
    settings,
    async cancellationToken =>
    {
        var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
        foreach (var fieldId in settings.FieldIds ?? [])
        {
            var created = await spots
                .GetOrCreate(fieldId, GameNames.FieldType)
                .InMesh(GameNames.WorldMesh)
                .Async(cancellationToken);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException($"Field bootstrap rejected: {fieldId}.");
        }
    }
);
