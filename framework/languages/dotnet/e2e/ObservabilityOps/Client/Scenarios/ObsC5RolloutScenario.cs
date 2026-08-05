// Verifies OBS-C5 Rollout behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC5RolloutScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        if (context.Options.C5Phase is "sequential" or "both")
            await RunSequentialAsync(context);
        if (context.Options.C5Phase is "simultaneous" or "both")
            await RunSimultaneousAsync(context);
        Console.WriteLine($"scenario OBS-C5 phase={context.Options.C5Phase} passed");
    }

    private static async Task RunSequentialAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-c5-sequential-{suffix}";
        var roomRid = $"room-c5-sequential-{suffix}";
        await context.PlayA.Post("/rooms").Body(new CreateRoomReq(roomRid)).AsyncRaw();
        await using var connector = await context.ConnectAsync();
        await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, roomRid);
        var lobby = await context.ReturnToEntrySpotAsync(
            connector, actorId, "obs-c5-room-complete");
        var sourceNode = lobby.NodeRid;
        _ = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        var targetNode = context.OtherPlayNode(sourceNode);
        var source = context.Play(sourceNode);
        await context.PlayA.Post($"/rooms/{roomRid}/close").AsyncRaw();
        await source.Post("/relocate?deadlineMs=30000").AsyncRaw();
        //  Relocation의 terminal 결과를 **먼저** 본다. `/relocate`는 시작만 하고
        //  200을 돌려주므로, actor 위치를 먼저 기다리면 relocation이 Blocked로
        //  끝났을 때도 조용한 timeout으로만 보인다.
        var status = await ScenarioContext.WaitForRelocationAsync(
            source, TimeSpan.FromSeconds(35));
        await WaitActorAsync(
            context, actorId, targetNode, TimeSpan.FromSeconds(15));
        ZlinkStreamAssert.Ensure(status.Result == "Relocated",
            $"OBS-C5 sequential rollout returned {status.Result}/{status.Reason}.");
        var metrics = (await source.Get("/evidence")
            .Async<EvidenceSnapshot>()).Body.Metrics;
        ZlinkStreamAssert.Ensure(
            metrics.All(sample =>
                sample.Name != "zlink.host.shutdown.forced"),
            "OBS-C5 sequential relocation started forced Shutdown.");
        await connector.Close.Async();
    }

    private static async Task RunSimultaneousAsync(ScenarioContext context)
    {
        _ = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        var suffix = Guid.NewGuid().ToString("N");
        var blockedRoomId = $"room-c5-blocked-{suffix}";
        //  Blocked fixture는 play-a가 아닌 노드에 올려야 한다. play-a는 이 시나리오
        //  내내 room 생성과 evidence 조회를 받는 관측자인데, relocate를 마친 host는
        //  spec 28 §204의 `Relocated`라 application admission이 봉인되어 이후 호출이
        //  전부 막힌다. 반대로 특정 노드로 못 박아도 안 된다 — sequential 단계가
        //  이미 한 노드를 relocate시켰을 수 있어 그 노드에는 배치가 되지 않는다.
        //  그래서 "play-a가 아닌 곳에 떨어질 때까지"로만 제한한다.
        var playANode = await context.PlayNodeIdAsync("play-a");
        CreateRoomRes blockedRoom;
        for (var attempt = 0; ; attempt++)
        {
            blockedRoom = (await context.PlayA.Post("/rooms")
                .Body(new CreateRoomReq($"{blockedRoomId}-{attempt}"))
                .Async<CreateRoomRes>()).Body;
            if (blockedRoom.NodeRid != playANode) break;
            await context.PlayA.Post($"/rooms/{blockedRoom.RoomRid}/close").AsyncRaw();
            if (attempt >= 63)
                throw new InvalidOperationException(
                    "OBS-C5 could not place the blocked fixture off the observer node.");
        }
        blockedRoomId = blockedRoom.RoomRid;
        var blockedNode = blockedRoom.NodeRid;
        var blockedHost = context.Play(blockedNode);

        await blockedHost.Post("/operation-gate/arm")
            .Query("maximumWaitMs", "30000").AsyncRaw();
        var blockedOperation = blockedHost.Post("/operation/start")
            .Body(new PlayBoundedOperationReq(
                blockedRoomId,
                "obs-c5-block-target"))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<PlayBoundedOperationRes>().AsTask();
        await blockedHost.Post("/operation-gate/wait-started")
            .Query("timeoutMs", "5000").AsyncRaw();

        //  Gate release는 relocate **이전**에 해야 한다. Relocate를 마친 host는
        //  spec 28 §204의 `Relocated`라 application admission이 봉인돼 있고, 그
        //  상태의 application endpoint 호출은 계약상 성공할 수 없다. 이 시나리오가
        //  검증하려는 것은 target 부적격이지 봉인된 host의 endpoint가 아니다.
        await blockedHost.Post("/operation-gate/release").AsyncRaw();
        var released = await blockedOperation;
        ZlinkStreamAssert.Ensure(released.Body.NodeRid == blockedNode,
            "OBS-C5 bounded handler did not complete on its observed owner.");
        await blockedHost.Post("/relocate?deadlineMs=30000").AsyncRaw();
        //  Config 11 OBS-C5는 "Source Host는 Serving이고"를 명시한다. Eligible
        //  target이 없으면 relocate는 preflight에서 `Blocked`로 끝나므로 host는
        //  `Relocating`으로 전이하지 않고, 따라서 topology의 draining 표식도 뜨지
        //  않는다. 그것을 기다리는 것은 이 시나리오가 검증하려는 계약과 정반대다.

        var actorId = $"obs-c5-zero-target-{suffix}";
        await using var connector = await context.ConnectAsync();
        var authenticated = await connector.Request(new AuthenticateReq(actorId))
            .Async<AuthenticateRes>();
        var sourceNode = authenticated.NodeRid;
        var sourceHost = context.Play(sourceNode);
        ZlinkStreamAssert.Ensure(sourceNode != blockedNode,
            "OBS-C5 placed new work on a node already marked for relocation.");

        // The placement loop may have created and closed a temporary room on
        // the observer node before it found the blocked node. Capture the
        // public evidence baseline so that only effects introduced by the two
        // preflight attempts are classified below.
        var beforePreflight = (await sourceHost.Get("/evidence")
            .Query("actorId", actorId)
            .Async<EvidenceSnapshot>()).Body;
        var planned = (await sourceHost.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 1000))
            .Async<RelocateHostRes>()).Body;
        var rolling = (await sourceHost.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "rolling-update", 99, 1000))
            .Async<RelocateHostRes>()).Body;
        var unchanged = (await sourceHost.Get("/evidence")
            .Query("actorId", actorId)
            .Async<EvidenceSnapshot>()).Body;
        ZlinkStreamAssert.Ensure(
            unchanged.ActorRows.Any(row =>
                row.ActorId == actorId && row.NodeRid == sourceNode),
            "OBS-C5 zero-target preflight changed Actor authority.");
        ZlinkStreamAssert.Ensure(
            planned is
            {
                Outcome: "Blocked",
                Reason: "TargetUnavailable"
            }
            && rolling is
            {
                Outcome: "Blocked",
                Reason: "TargetUnavailable"
            },
            $"OBS-C5 zero-target modes returned planned="
            + $"{planned.Outcome}/{planned.Reason}, rolling="
            + $"{rolling.Outcome}/{rolling.Reason}.");
        var sourceStatus = (await sourceHost.Get("/runtime/status")
            .Async<RuntimeStatusRes>()).Body;
        ZlinkStreamAssert.Ensure(
            sourceStatus is
            {
                State: "Serving",
                IsReady: true,
                AcceptingWork: true
            },
            "OBS-C5 preflight blocker changed source readiness.");
        //  Config 11 OBS-C5의 "follow-up request가 성공한다"는 그 시점 상태에서
        //  처리 가능한 요청이어야 한다. `GameActionReq`는 `RoomSpot`의 actor
        //  handler라 room에 들어가 있어야 하는데 이 actor는 lobby에 있다. Entry
        //  Spot이 받는 요청으로 확인한다.
        var serveRoom = await context.CreateRoomOnObservedNodeAsync(
            sourceNode, $"room-c5-still-serving-{suffix}");
        var serving = await context.JoinRoomAsync(
            connector, actorId, serveRoom.RoomRid);
        ZlinkStreamAssert.Ensure(
            serving.NodeRid == sourceNode,
            "OBS-C5 handler admission did not remain on the source.");
        //  두 부작용이 한 단언에 묶여 있어 어느 쪽이 위반인지 알 수 없었다.
        var closingEntries = unchanged.Entries
            .Skip(beforePreflight.Entries.Length)
            .Where(entry => entry.Contains("spot-closing", StringComparison.Ordinal))
            .ToArray();
        var startedBefore = beforePreflight.Metrics
            .Where(sample => sample.Name == "zlink.relocation.started")
            .Sum(sample => sample.Value);
        var startedAfter = unchanged.Metrics
            .Where(sample => sample.Name == "zlink.relocation.started")
            .Sum(sample => sample.Value);
        ZlinkStreamAssert.Ensure(
            closingEntries.Length == 0 && startedAfter == startedBefore,
            $"OBS-C5 preflight blocker created relocation side effects. "
            + $"closing=[{string.Join(";", closingEntries)}] "
            + $"started_before={startedBefore} started_after={startedAfter}");
    }


    private static async Task WaitActorAsync(
        ScenarioContext context, string actorId, string nodeRid, TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            //  관측은 계속 serving인 target에게 묻는다. `PlayB`로 못 박으면
            //  source가 play-b일 때 relocate 중인 노드에 물어보게 된다.
            var snapshot = (await context.Play(nodeRid).Get("/evidence")
                .Query("actorId", actorId)
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.ActorRows.Any(row => row.ActorId == actorId && row.NodeRid == nodeRid)) return;
            await Task.Delay(100);
        }
        throw new TimeoutException("OBS-C5 sequential actor location did not converge.");
    }

}
