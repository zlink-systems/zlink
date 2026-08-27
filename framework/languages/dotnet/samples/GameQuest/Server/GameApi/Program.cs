using Microsoft.Extensions.Configuration;

using GameQuest.GameApi.Application;
using GameQuest.GameApi.Infrastructure.Store;
using GameQuest.GameApi.Infrastructure.ZLink;
using GameQuest.GameApi.Session;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Microsoft.AspNetCore.Mvc;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace GameQuest.GameApi;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = GameQuestTopology.LoadGameApi(args);
        var topology = configuration.Topology;
        var apiName = configuration.InstanceName;
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.WebHost.UseUrls(
            string.Equals(apiName, "api-b", StringComparison.Ordinal)
                ? topology.GameApiBHttpBaseUrl
                : topology.GameApiAHttpBaseUrl);
        SampleLogging.Configure(
            builder.Logging,
            configuration.LogDirectory,
            apiName);

        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton<GameQuestStore>();
        builder.Services.AddSingleton<IGameplayEventStore>(sp => sp.GetRequiredService<GameQuestStore>());
        builder.Services.AddSingleton<IQuestSessionStore>(sp => sp.GetRequiredService<GameQuestStore>());
        builder.Services.AddSingleton<IGameplayEventOwnerDispatcher, GameplayEventOwnerDispatcher>();
        builder.Services.AddSingleton<IQuestProgressSynchronizer, ZLinkQuestProgressSynchronizer>();
        builder.Services.AddScoped<GameplayActionService>();
        builder.Services.AddScoped<JoinQuestSessionUseCase>();
        builder.Services.AddScoped<GameQuestSession>();
        builder.Services.AddScoped<GetQuestProgressHandler>();
        builder.Services.AddScoped<SyncQuestProgressHandler>();
        builder.Services.AddSingleton(new GameQuestSpotRouteReadiness(
            apiName,
            SampleNames.MeshName));
        builder.Services.AddHostedService<GameQuestSpotRouteReadinessReporter>();
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
                .Listen(topology.GameApiMeshEndpoint(apiName))
                .SetRoutingIdPrefix("game-api");
            mesh.Objects().Server()
                .AddEntrySpot<GameQuestEntrySpot>()
                .AddActorFactory<PlayerSessionActor, PlayerSessionActorFactory>(
                    SampleNames.SessionActorType, factory => factory.RecreateOnRelocation());
            options.AddStreamNode(SampleNames.StreamNode)
                .Bind(configuration.StreamBindEndpoint)
                .EnableActorDispatch()
                .AddSession<GameQuestSession>();
        });

        var app = builder.Build();

        app.MapGet("/health", () => Results.Ok(new { status = "ok" }));
        app.Lifetime.ApplicationStarted.Register(() =>
            app.Logger.LogInformation(
                "gamequest-ready kind=stream node={NodeId}",
                apiName));

        app.MapGet("/quest/progress/{playerId}", async (
            string playerId,
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            var response = new GetQuestProgressRes(await store.ReadProjectionAsync(playerId, cancellationToken));
            return Results.Ok(response);
        });

        app.MapPost("/self-check/gameplay/kill-without-publish/{playerId}", async (
            string playerId,
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            await store.AddUnpublishedKillAsync(playerId, 1, cancellationToken);
            return Results.Ok(new { accepted = true });
        });

        app.MapGet("/self-check/gameplay/snapshot/{playerId}", async (
            string playerId,
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            return Results.Ok(await store.ReadSnapshotAsync(playerId, cancellationToken));
        });

        app.MapPost("/self-check/gameplay/collect/{playerId}/{itemId}/{count:int}/{idempotencyKey}", async (
            string playerId,
            string itemId,
            int count,
            string idempotencyKey,
            GameplayActionService actions,
            CancellationToken cancellationToken) =>
        {
            var eventId = await actions.CollectItemAsync(
                playerId,
                itemId,
                count,
                idempotencyKey,
                cancellationToken);
            return Results.Ok(new { accepted = true, eventId });
        });

        app.MapPost("/self-check/sync/{playerId}", async (
            string playerId,
            IQuestProgressSynchronizer quests,
            CancellationToken cancellationToken) =>
        {
            return Results.Ok(await quests.SyncAsync(playerId, cancellationToken));
        });

        app.MapPost("/self-check/projection/{playerId}/{questId}/delete", async (
            string playerId,
            string questId,
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            await store.DeleteProjectionAsync(playerId, questId, cancellationToken);
            return Results.Ok(new { deleted = true });
        });

        app.MapPost("/self-check/projection/{playerId}/{questId}/rebuild", async (
            string playerId,
            string questId,
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            return Results.Ok(await store.RebuildProjectionAsync(playerId, questId, cancellationToken));
        });

        app.MapPost("/self-check/assert", async (
            GameQuestStore store,
            CancellationToken cancellationToken) =>
        {
            var alice = await store.ReadProjectionAsync("player-alice", cancellationToken);
            var bob = await store.ReadProjectionAsync("player-bob", cancellationToken);
            var evidence = alice.Concat(bob)
                .Select(p => $"{p.PlayerId}:{p.QuestId}:{p.Status}:{p.CurrentCount}/{p.RequiredCount}")
                .Order(StringComparer.Ordinal)
                .ToArray();
            var events = await store.ReadQuestEventsAsync(cancellationToken);
            var rehydrates = await store.ReadOwnerRehydrateEvidenceAsync(cancellationToken);
            var passed = alice.Any(p => p is { QuestId: QuestIds.FirstHunt, Status: QuestStatuses.RewardGranted })
                         && alice.Any(p => p is { QuestId: QuestIds.VisitRuins, Status: QuestStatuses.RewardGranted })
                         && bob.Any(p => p is { QuestId: QuestIds.HerbGathering, Status: QuestStatuses.RewardGranted })
                         && Count(events, "player-alice", QuestIds.FirstHunt, nameof(QuestProgressedEvent)) == 3
                         && Count(events, "player-alice", QuestIds.FirstHunt, nameof(QuestCompletedEvent)) == 1
                         && Count(events, "player-alice", QuestIds.FirstHunt, nameof(QuestRewardGrantedEvent)) == 1
                         && Count(events, "player-alice", QuestIds.FirstHunt, nameof(QuestReconciled)) == 1
                         && Count(events, "player-alice", QuestIds.VisitRuins, nameof(QuestCompletedEvent)) == 1
                         && Count(events, "player-alice", QuestIds.VisitRuins, nameof(QuestRewardGrantedEvent)) == 1
                         && Count(events, "player-bob", QuestIds.HerbGathering, nameof(QuestCompletedEvent)) == 1
                         && Count(events, "player-bob", QuestIds.HerbGathering, nameof(QuestRewardGrantedEvent)) == 1
                         && rehydrates.GetValueOrDefault("player-alice") >= 2
                         && rehydrates.GetValueOrDefault("player-bob") >= 1
                         && events
                             .GroupBy(e => (e.PlayerId, e.QuestId, e.Version))
                             .All(group => group.Count() == 1);
            return Results.Ok(new
            {
                passed,
                evidence = evidence.Concat(events.Select(e =>
                        $"event:{e.PlayerId}:{e.QuestId}:{e.Type}:v{e.Version}:source={e.SourceEventId}"))
                    .Concat(rehydrates.Select(pair => $"rehydrated:{pair.Key}:{pair.Value}"))
                    .Order(StringComparer.Ordinal)
                    .ToArray()
            });
        });

        await app.RunAsync();
    }

    private static int Count(
        IEnumerable<StoredQuestEvent> events,
        string playerId,
        string questId,
        string eventType)
    {
        return events.Count(e =>
            e.PlayerId == playerId
            && e.QuestId == questId
            && e.Type == eventType);
    }

}
