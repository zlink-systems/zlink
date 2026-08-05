using GameQuest.Client.Configuration;
using GameQuest.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace GameQuest.Client;

internal sealed class GameQuestClientScenario(GameQuestTopology topology)
{
    // End-to-end client story:
    // 1. Subscribe player-alice on API A and verify quest progress/completion from combat events.
    // 2. Replay a duplicate event and confirm the same event id is treated idempotently.
    // 3. Send one-way item and area actions and verify their progress notifications.
    // 4. Create offline progress for player-bob, reconnect on API B, and finish the herb quest.
    // 5. Delete and rebuild a projection to prove stream queries see recovered quest state.
    // 6. Reconcile a missed publish through sync, then ask the server for final evidence.
    public async ValueTask RunAsync(
        IZlinkStreamConnector apiAStream,
        IZlinkStreamConnector apiBStream,
        CancellationToken cancellationToken = default)
    {
        using var apiA = ZLinkHttpClient.Create(topology.GameApiAHttpBaseUrl).Build();
        using var apiB = ZLinkHttpClient.Create(topology.GameApiBHttpBaseUrl).Build();

        await apiAStream.Connect.Async(cancellationToken);
        var joined = await apiAStream.Request(new JoinSessionReq("player-alice"))
            .Async<JoinSessionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(joined.PlayerId == "player-alice", "Join response must identify player-alice.");
        ZlinkStreamAssert.Ensure(joined.ActiveQuests.Length == 0, "Assertion failed: joined.ActiveQuests.Length == 0");

        var firstProgress = apiAStream.WaitFor<QuestProgressNotify>().Async(cancellationToken);
        var firstKill = await apiAStream.Request(new KillMonsterReq("player-alice", "wolf", "forest", "kill-1"))
            .Async<KillMonsterRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(firstKill.EventId == "player-alice-kill-1", "Assertion failed: firstKill.EventId == \"player-alice-kill-1\"");
        var firstProgressPush = await firstProgress;
        ZlinkStreamAssert.Ensure(firstProgressPush.Payload.PlayerId == "player-alice", "Assertion failed: firstProgressPush.Payload.PlayerId == \"player-alice\"");
        ZlinkStreamAssert.Ensure(firstProgressPush.Payload.Progress.QuestId == QuestIds.FirstHunt, "Assertion failed: firstProgressPush.Payload.Progress.QuestId == QuestIds.FirstHunt");
        ZlinkStreamAssert.Ensure(firstProgressPush.Payload.Progress.CurrentCount == 1, "Assertion failed: firstProgressPush.Payload.Progress.CurrentCount == 1");

        var completeFirstHunt = apiAStream.WaitFor<QuestCompletedNotify>().Async(cancellationToken);
        _ = await apiAStream.Request(new KillMonsterReq("player-alice", "wolf", "forest", "kill-2"))
            .Async<KillMonsterRes>(cancellationToken);
        var thirdKill = await apiAStream.Request(new KillMonsterReq("player-alice", "wolf", "forest", "kill-3"))
            .Async<KillMonsterRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(thirdKill.EventId == "player-alice-kill-3", "Assertion failed: thirdKill.EventId == \"player-alice-kill-3\"");
        var completeFirstHuntPush = await completeFirstHunt;
        ZlinkStreamAssert.Ensure(completeFirstHuntPush.Payload.PlayerId == "player-alice", "Assertion failed: completeFirstHuntPush.Payload.PlayerId == \"player-alice\"");
        ZlinkStreamAssert.Ensure(completeFirstHuntPush.Payload.Progress.QuestId == QuestIds.FirstHunt, "Assertion failed: completeFirstHuntPush.Payload.Progress.QuestId == QuestIds.FirstHunt");
        ZlinkStreamAssert.Ensure(completeFirstHuntPush.Payload.RewardGranted, "Assertion failed: completeFirstHuntPush.Payload.RewardGranted");

        var duplicate = await apiAStream.Request(new KillMonsterReq("player-alice", "wolf", "forest", "kill-3"))
            .Async<KillMonsterRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(duplicate.EventId == thirdKill.EventId, "Assertion failed: duplicate.EventId == thirdKill.EventId");

        using var mission = ZLinkHttpClient.Create(topology.MissionAHttpBaseUrl).Build();
        var closeOwner = await mission.Post("/self-check/owner/player-alice/close")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(closeOwner.Status is >= 200 and < 300, "Assertion failed: closeOwner.Status is >= 200 and < 300");
        var rehydratedAfterClose = await apiAStream.Request(new SyncQuestProgressReq("player-alice"))
            .Async<SyncQuestProgressRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(
            rehydratedAfterClose.UpdatedQuests.Any(progress =>
                progress is { QuestId: QuestIds.FirstHunt, Status: QuestStatuses.RewardGranted }),
            "Assertion failed: rehydratedAfterClose.UpdatedQuests contains rewarded first-hunt");

        var ruinsCompleted = apiAStream.WaitFor<QuestCompletedNotify>().Async(cancellationToken);
        await apiAStream.Send(new EnterAreaReq("player-alice", "ruins", "enter-ruins"))
            .Async(cancellationToken);
        var ruinsCompletedPush = await ruinsCompleted;
        ZlinkStreamAssert.Ensure(ruinsCompletedPush.Payload.PlayerId == "player-alice", "Assertion failed: ruinsCompletedPush.Payload.PlayerId == \"player-alice\"");
        ZlinkStreamAssert.Ensure(ruinsCompletedPush.Payload.Progress.QuestId == QuestIds.VisitRuins, "Assertion failed: ruinsCompletedPush.Payload.Progress.QuestId == QuestIds.VisitRuins");
        ZlinkStreamAssert.Ensure(ruinsCompletedPush.Payload.RewardGranted, "Assertion failed: ruinsCompletedPush.Payload.RewardGranted");

        // Simulate an authoritative server event while Bob has no bound session.
        var offlineItem = await apiA.Post("/self-check/gameplay/collect/player-bob/healing-herb/1/herb-1")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(offlineItem.Status is >= 200 and < 300, "Assertion failed: offlineItem.Status is >= 200 and < 300");

        await apiBStream.Connect.Async(cancellationToken);
        var bobJoined = await apiBStream.Request(new JoinSessionReq("player-bob"))
            .Async<JoinSessionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(bobJoined.PlayerId == "player-bob", "Join response must identify player-bob.");
        var bobProgress = bobJoined.ActiveQuests.Any(p =>
            p is { QuestId: QuestIds.HerbGathering, CurrentCount: 1 })
            ? bobJoined.ActiveQuests
            : await WaitForStreamProjectionAsync(
                apiBStream,
                "player-bob",
                progress => progress is { QuestId: QuestIds.HerbGathering, CurrentCount: 1 },
                cancellationToken);
        ZlinkStreamAssert.Ensure(bobProgress.Any(p => p is { QuestId: QuestIds.HerbGathering, CurrentCount: 1 }), "Assertion failed: bobProgress.Any(p => p is { QuestId: QuestIds.HerbGathering, CurrentCount: 1 })");
        var herbCompletedOnReconnectedStream = apiBStream.WaitFor<QuestCompletedNotify>().Async(cancellationToken);
        await apiBStream.Send(new CollectItemReq("player-bob", "healing-herb", 4, "herb-2"))
            .Async(cancellationToken);
        var herbCompletedOnReconnectedStreamPush = await herbCompletedOnReconnectedStream;
        ZlinkStreamAssert.Ensure(herbCompletedOnReconnectedStreamPush.Payload.PlayerId == "player-bob", "Assertion failed: herbCompletedOnReconnectedStreamPush.Payload.PlayerId == \"player-bob\"");
        ZlinkStreamAssert.Ensure(herbCompletedOnReconnectedStreamPush.Payload.Progress.QuestId == QuestIds.HerbGathering, "Assertion failed: herbCompletedOnReconnectedStreamPush.Payload.Progress.QuestId == QuestIds.HerbGathering");
        ZlinkStreamAssert.Ensure(herbCompletedOnReconnectedStreamPush.Payload.RewardGranted, "Assertion failed: herbCompletedOnReconnectedStreamPush.Payload.RewardGranted");
        ZlinkStreamAssert.Ensure(herbCompletedOnReconnectedStreamPush.Payload.Progress.Status == QuestStatuses.RewardGranted, "Assertion failed: herbCompletedOnReconnectedStreamPush.Payload.Progress.Status == QuestStatuses.RewardGranted");

        var deleteBobProjection = await apiA.Post($"/self-check/projection/player-bob/{QuestIds.HerbGathering}/delete")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(deleteBobProjection.Status is >= 200 and < 300, "Assertion failed: deleteBobProjection.Status is >= 200 and < 300");
        var missingProjection = await GetStreamProjectionAsync(apiBStream, "player-bob", cancellationToken);
        ZlinkStreamAssert.Ensure(missingProjection.All(progress => progress.QuestId != QuestIds.HerbGathering), "Assertion failed: missingProjection.All(progress => progress.QuestId != QuestIds.HerbGathering)");
        var rebuilt = await apiA.Post($"/self-check/projection/player-bob/{QuestIds.HerbGathering}/rebuild")
            .Fetch<QuestProgress>(cancellationToken);
        ZlinkStreamAssert.Ensure(rebuilt is { QuestId: QuestIds.HerbGathering, Status: QuestStatuses.RewardGranted }, "Assertion failed: rebuilt is { QuestId: QuestIds.HerbGathering, Status: QuestStatuses.RewardGranted }");
        var rebuiltProjection = await GetStreamProjectionAsync(apiBStream, "player-bob", cancellationToken);
        ZlinkStreamAssert.Ensure(rebuiltProjection.Any(progress =>
            progress is { QuestId: QuestIds.HerbGathering, Status: QuestStatuses.RewardGranted }), "Assertion failed: rebuiltProjection.Any(progress => progress is { QuestId: QuestIds.HerbGathering, Status: QuestStatuses.RewardGranted })");

        var killWithoutPublish = await apiB.Post("/self-check/gameplay/kill-without-publish/player-alice")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(killWithoutPublish.Status is >= 200 and < 300, "Assertion failed: killWithoutPublish.Status is >= 200 and < 300");
        var sync = await apiAStream.Request(new SyncQuestProgressReq("player-alice"))
            .Async<SyncQuestProgressRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(sync.UpdatedQuests.Any(progress =>
            progress.QuestId == QuestIds.FirstHunt && progress.CurrentCount >= 4), "Assertion failed: sync.UpdatedQuests.Any(progress => progress.QuestId == QuestIds.FirstHunt && progress.CurrentCount >= 4)");
        var reconciled = await WaitForProjectionAsync(
            apiB,
            "player-alice",
            progress => progress.QuestId == QuestIds.FirstHunt && progress.CurrentCount >= 4,
            cancellationToken);
        ZlinkStreamAssert.Ensure(reconciled.Any(p => p.QuestId == QuestIds.FirstHunt && p.CurrentCount >= 4), "Assertion failed: reconciled.Any(p => p.QuestId == QuestIds.FirstHunt && p.CurrentCount >= 4)");

        await apiAStream.DisposeAsync();

        var assertion = await WaitForServerAssertionAsync(apiA, cancellationToken);
        ZlinkStreamAssert.Ensure(assertion.Passed, "Assertion failed: assertion.Passed");
    }

    private async ValueTask<ServerAssertionResponse> WaitForServerAssertionAsync(
        ZLinkHttpClient api,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + SampleNames.RequestTimeout;
        ServerAssertionResponse? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            last = await api.Post("/self-check/assert")
                .Fetch<ServerAssertionResponse>(cancellationToken);
            if (last.Passed) return last;

            await Task.Delay(50, cancellationToken);
        }

        return last ?? throw new InvalidOperationException("Server assertion did not return evidence.");
    }

    private async ValueTask<QuestProgress[]> WaitForProjectionAsync(
        ZLinkHttpClient api,
        string playerId,
        Func<QuestProgress, bool> predicate,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + SampleNames.RequestTimeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var response = await api.Get($"/quest/progress/{playerId}")
                .Fetch<GetQuestProgressRes>(cancellationToken);
            if (response.ActiveQuests.Any(predicate)) return response.ActiveQuests;

            await Task.Delay(50, cancellationToken);
        }

        return (await api.Get($"/quest/progress/{playerId}")
            .Fetch<GetQuestProgressRes>(cancellationToken)).ActiveQuests;
    }

    private static async ValueTask<QuestProgress[]> WaitForStreamProjectionAsync(
        IZlinkStreamConnector connector,
        string playerId,
        Func<QuestProgress, bool> predicate,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + SampleNames.RequestTimeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var response = await connector.Request(new GetQuestProgressReq(playerId))
                .Async<GetQuestProgressRes>(cancellationToken);
            if (response.ActiveQuests.Any(predicate)) return response.ActiveQuests;

            await Task.Delay(50, cancellationToken);
        }

        return (await connector.Request(new GetQuestProgressReq(playerId))
            .Async<GetQuestProgressRes>(cancellationToken)).ActiveQuests;
    }

    private static async ValueTask<QuestProgress[]> GetStreamProjectionAsync(
        IZlinkStreamConnector connector,
        string playerId,
        CancellationToken cancellationToken)
    {
        return (await connector.Request(new GetQuestProgressReq(playerId))
            .Async<GetQuestProgressRes>(cancellationToken)).ActiveQuests;
    }
}

internal sealed record ServerAssertionResponse(bool Passed, string[] Evidence);
