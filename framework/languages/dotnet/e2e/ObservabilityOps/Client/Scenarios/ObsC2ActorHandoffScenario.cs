// Verifies OBS-C2 Actor Handoff behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC2ActorHandoffScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-c2-{suffix}";
        var roomRid = $"room-c2-{suffix}";
        //  `start_play_b`는 play-b를 항상 `--metrics-enabled false`로 띄운다.
        //  Actor가 play-b에 착지하면 source의 `/evidence` metric이 통째로 비어
        //  이 scenario의 metric 검증이 배치 운에 좌우된다. 첫 room을 play-a에
        //  고정해 source를 metric이 있는 쪽으로 못 박는다.
        var playANode = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        // Actor creation is weighted across the mesh, while this scenario's
        // precondition requires the initial Entry Spot to be play-a. Keep
        // play-b eligible for the later host relocation, but exclude it only
        // during the initial authentication.
        await context.SetPlayPlacementWeightAsync("play-b", 0);
        var room = await context.CreateRoomOnObservedNodeAsync(
            playANode, $"room-c2-{suffix}");
        roomRid = room.RoomRid;
        await using var connector = await context.ConnectAsync();
        await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, roomRid);
        //  Room join은 actor를 room spot 소유 node로 옮기므로, join completion 직후
        //  창에서는 binding admission이 `RetryAfterBackoff`로 거절할 수 있다.
        //  런타임이 재시도를 지시하는 상태이므로 예산 안에서 다시 보낸다.
        async Task<GameActionRes> SendPendingAsync(int index)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            Exception? last = null;
            while (DateTime.UtcNow < deadline)
            {
                try
                {
                    return await connector
                        .Request(new GameActionReq($"obs-c2-pending-{index}", 100))
                        .Async<GameActionRes>();
                }
                catch (Exception error)
                {
                    last = error;
                    await Task.Delay(TimeSpan.FromMilliseconds(200));
                }
            }

            throw new InvalidOperationException(
                $"OBS-C2 pending request {index} was never admitted: {last?.Message}");
        }

        var requests = Enumerable.Range(0, 3)
            .Select(index => SendPendingAsync(index)).ToArray();
        var replies = await Task.WhenAll(requests);
        ZlinkStreamAssert.Ensure(replies.All(reply => reply.ActorId == actorId),
            "OBS-C2 a request lost its original reply during handoff.");
        var lobby = await context.ReturnToEntrySpotAsync(
            connector, actorId, "obs-c2-room-complete");
        ZlinkStreamAssert.Ensure(lobby.SpotId is null,
            "OBS-C2 actor did not leave the completed room.");
        var sourceNode = lobby.NodeRid;
        ZlinkStreamAssert.Ensure(sourceNode == playANode,
            $"OBS-C2 actor did not stay on the metrics-enabled node: "
            + $"{sourceNode} != {playANode}.");
        await context.SetPlayPlacementWeightAsync("play-b", 100);
        var targetNode = context.OtherPlayNode(sourceNode);
        var source = context.Play(sourceNode);
        //  이동 뒤 연속성 확인에 쓸 room은 두 노드가 모두 성한 지금 target에
        //  올려 둔다. Relocate가 시작된 뒤에는 source의 `/rooms`가 배치를
        //  받아주지 않아 room이 draining 노드로 떨어질 수 있다.
        var movedRoom = await context.CreateRoomOnObservedNodeAsync(
            targetNode, $"room-c2-moved-{suffix}");
        await context.PlayA.Post($"/rooms/{roomRid}/close").AsyncRaw();
        await source.Post("/relocate?deadlineMs=30000").AsyncRaw();
        //  Relocation의 terminal 결과를 **먼저** 본다. `/relocate`는 시작만 하고
        //  200을 돌려주므로, actor 위치를 먼저 기다리면 relocation이 Blocked로
        //  끝났을 때도 조용한 timeout으로만 보인다. 결과를 먼저 드러내면 실패
        //  이유가 메시지에 실린다.
        var relocation = await ScenarioContext.WaitForRelocationAsync(
            source, TimeSpan.FromSeconds(40));
        ZlinkStreamAssert.Ensure(relocation.Result == "Relocated",
            $"OBS-C2 host relocation did not complete: {relocation.Result}/{relocation.Reason}.");
        var location = await WaitActorLocationAsync(
            context, actorId, targetNode);
        ZlinkStreamAssert.Ensure(
            location.ActorRows.Any(row =>
                row.ActorId == actorId && row.NodeRid == targetNode),
            "OBS-C2 actor location did not commit to the eligible peer.");
        var result = relocation;
        //  In-flight histogram은 관측 창이 지나면 사라진다. 이동 직후의 스냅샷에서
        //  읽어 둔다. Terminal counter는 아래에서 따로 예산을 두고 읽는다.
        var servingMetrics = (await source.Get("/evidence")
            .Async<EvidenceSnapshot>()).Body.Metrics;
        ZlinkStreamAssert.Ensure(result.Result == "Relocated",
            $"OBS-C2 serving target handoff failed: {result.Result}/{result.Reason}.");
        //  이동 뒤 연속성은 `PlayerMovedNotify`로 확인할 수 없다. Spec 28 §536~540이
        //  "이 이동은 application이 요청한 join이 아니므로 target Entry Spot의
        //  `OnJoinedActor`를 호출하지 않는다"고 정하는데, 그 push는 바로 그 훅에서
        //  나간다(`Server/Play/Spots/PlayEntrySpot.cs`). 즉 계약상 오지 않는다.
        //  Config 11 OBS-C2가 요구하는 것은 "bound session이 이동 뒤에도 target
        //  Actor로 이어진다"이므로, 같은 session으로 새 room join을 태워서 그
        //  push가 target에서 돌아오는지로 확인한다. 이쪽 join은 application이
        //  요청한 join이라 `OnJoinedActor`가 정상적으로 불린다.
        //  이동 직후에는 target에서 본 room의 위치 행이 아직 안 보일 수 있다.
        //  Join 실패는 예외가 아니라 완료 통지로 오므로 여기서 예산을 두고
        //  다시 시도한다.
        ActorJoinCompletedNotify? rejoined = null;
        var rejoinDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        Exception? lastRejoinError = null;
        while (DateTime.UtcNow < rejoinDeadline)
        {
            try
            {
                rejoined = await context.JoinRoomAsync(
                    connector, actorId, movedRoom.RoomRid);
                break;
            }
            catch (Exception error)
            {
                lastRejoinError = error;
                await Task.Delay(TimeSpan.FromMilliseconds(500));
            }
        }
        if (rejoined is null)
        {
            //  실패하면 actor와 room이 실제로 어디에 있는지 함께 드러낸다.
            var placement = (await context.Play(targetNode).Get("/evidence")
                .Async<EvidenceSnapshot>()).Body;
            var actorRows = string.Join(
                ",",
                placement.ActorRows.Select(row => $"{row.ActorId}@{row.NodeRid}"));
            var spotRows = string.Join(
                ",",
                placement.SpotRows.Select(row => $"{row.SpotRid}@{row.NodeRid}"));
            ZlinkStreamAssert.Ensure(false,
                $"OBS-C2 bound session could not reach the relocated actor: "
                + $"{lastRejoinError?.Message} source={sourceNode} target={targetNode} "
                + $"room={movedRoom.RoomRid}@{movedRoom.NodeRid} "
                + $"actors=[{actorRows}] spots=[{spotRows}]");
        }
        ZlinkStreamAssert.Ensure(rejoined!.NodeRid == targetNode,
            $"OBS-C2 bound session did not continue to the relocated actor: "
            + $"{rejoined.NodeRid} != {targetNode}.");
        var movedLocation = await WaitActorLocationAsync(context, actorId, targetNode);
        ZlinkStreamAssert.Ensure(
            movedLocation.ActorRows.Any(row =>
                row.ActorId == actorId && row.NodeRid == targetNode),
            "OBS-C2 relocated actor did not stay committed on the target owner.");
        //  Metric은 relocation의 마지막에 기록된다. `/relocate`가 terminal 결과를
        //  돌려준 직후 한 번만 읽으면 아직 안 올라와 있을 수 있으므로, 여기서
        //  예산을 두고 다시 읽는다.
        var metrics = Array.Empty<MetricSample>() as IReadOnlyList<MetricSample>;
        var metricDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTime.UtcNow < metricDeadline)
        {
            metrics = (await source.Get("/evidence")
                .Async<EvidenceSnapshot>()).Body.Metrics;
            if (metrics.Any(sample =>
                    sample.Name == "zlink.relocation.completed"
                    && sample.Tags.GetValueOrDefault("object_kind") == "actor"
                    && sample.Tags.GetValueOrDefault("outcome") == "completed"
                    && sample.Value >= 1))
                break;
            await Task.Delay(TimeSpan.FromMilliseconds(500));
        }
        ZlinkStreamAssert.Ensure(metrics.Any(sample =>
                sample.Name == "zlink.relocation.completed"
                && sample.Tags.GetValueOrDefault("object_kind") == "actor"
                && sample.Tags.GetValueOrDefault("outcome") == "completed"
                && sample.Value >= 1),
            "OBS-C2 Actor relocation completion metric did not increase. observed=["
            + string.Join(
                ";",
                metrics.Where(sample => sample.Name.StartsWith("zlink.relocation"))
                    .Select(sample =>
                        $"{sample.Name}{{"
                        + string.Join(
                            ",", sample.Tags.Select(tag => $"{tag.Key}={tag.Value}"))
                        + $"}}={sample.Value}"))
            + "]");
        ZlinkStreamAssert.Ensure(servingMetrics.Any(sample =>
                sample.Name == "zlink.mesh_node.requests.inflight"
                && sample.Tags.GetValueOrDefault("surface") == "actor"
                && sample.Count >= 1),
            "OBS-C2 in-flight Actor request was not observed.");
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-C2 passed");
    }

    private static async Task<EvidenceSnapshot> WaitActorLocationAsync(
        ScenarioContext context,
        string actorId,
        string targetNode)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            //  Evidence는 **이동 대상 node**에서 읽어야 한다. `targetNode`는
            //  `OtherPlayNode(sourceNode)`로 계산되므로 room 배치에 따라 play-a가
            //  될 수 있는데, 여기서 `PlayB`를 하드코딩하면 그때 영원히 수렴하지
            //  않는다.
            var snapshot = (await context.Play(targetNode).Get("/evidence")
                .Query("actorId", actorId)
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.ActorRows.Any(row =>
                    row.ActorId == actorId && row.NodeRid == targetNode))
                return snapshot;
            await Task.Delay(100);
        }
        throw new TimeoutException("OBS-C2 actor handoff location did not converge.");
    }

}
