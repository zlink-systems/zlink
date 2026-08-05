using Microsoft.Extensions.Configuration;

using GameQuest.QuestMission.Application;
using GameQuest.QuestMission.Infrastructure.Store;
using GameQuest.QuestMission.Infrastructure.ZLink;
using GameQuest.QuestMission.Infrastructure.ZLink.Spots.PlayerQuestSpot;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Spots;
using Zlink.Samples.Logging;

namespace GameQuest.QuestMission;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = GameQuestTopology.LoadQuestMission(args);
        var topology = configuration.Topology;
        var missionName = configuration.InstanceName;
        var instance = topology.ForQuestMission(missionName);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.WebHost.UseUrls(topology.MissionHttpBaseUrl(missionName));
        SampleLogging.Configure(
            builder.Logging,
            configuration.LogDirectory,
            missionName);

        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(instance);
        builder.Services.AddSingleton<QuestStore>();
        builder.Services.AddSingleton<IQuestStore>(sp => sp.GetRequiredService<QuestStore>());
        builder.Services.AddSingleton<IGameApiSnapshotClient, HttpGameApiSnapshotClient>();
        builder.Services.AddSingleton<IQuestProgressNotifier, ZLinkQuestProgressNotifier>();
        builder.Services.AddSingleton(new QuestProcessorIdentity(missionName));
        builder.Services.AddScoped<QuestEventProcessor>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = $"{topology.RedisKeyPrefix}relocation:";
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(Program));
            var mesh = options.AddRouteMesh(SampleNames.MeshName)
                .Listen(instance.MeshEndpoint)
                .SetRoutingIdPrefix("quest-mission");
            mesh.Objects().Server().AddInstanceSpotFactory<PlayerQuestSpot>(
                SampleNames.PlayerQuestSpotType, factory => factory.RecreateOnRelocation());
        });

        var app = builder.Build();

        app.MapGet("/health", () => Results.Ok(new { status = "ok", mission = instance.MissionName }));
        app.MapGet("/self-check/events",
            async (QuestStore store, CancellationToken cancellationToken) =>
            {
                return Results.Ok(await store.ReadStoredEventsAsync(cancellationToken));
            });
        app.MapPost("/self-check/owner/{playerId}/close", async (
            string playerId,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            await spots.SendToSpot(playerId, new ClosePlayerQuestMsg())
                .Async(cancellationToken);
            return Results.Ok();
        });
        await app.RunAsync();
    }
}
