// Verifies OBS-C3 User Spot aggregate relocation and stale global Spot ID behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC3UserSpotAggregateRelocationScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-c3-{suffix}";
        var roomId = $"room-c3-{suffix}";
        var room = (await context.PlayA.Post("/rooms")
            .Body(new CreateRoomReq(roomId, "fixed-drain"))
            .Async<CreateRoomRes>()).Body;
        var sourceNode = room.NodeRid;
        _ = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        var targetNode = context.OtherPlayNode(sourceNode);
        var source = context.Play(sourceNode);
        var target = context.Play(targetNode);

        await using var connector = await context.ConnectAsync();
        var authenticated = await connector.Request(new AuthenticateReq(actorId))
            .Async<AuthenticateRes>();
        var joined = await context.JoinRoomAsync(connector, actorId, roomId);
        ZlinkStreamAssert.Ensure(
            joined.NodeRid == sourceNode,
            "OBS-C3 Actor did not join the observed room owner.");

        var before = await WaitPlayAggregateAsync(
            target, roomId, actorId, sourceNode, TimeSpan.FromSeconds(10));
        var roomGeneration = before.SpotRows
            .Single(row => row.SpotRid == roomId).Generation;
        var actorGeneration = before.ActorRows
            .Single(row => row.ActorId == actorId).Generation;
        ZlinkStreamAssert.Ensure(
            roomGeneration > 0
            && actorGeneration == checked((long)authenticated.Generation),
            "OBS-C3 initial aggregate generations were not published.");

        await source.Post("/operation-gate/arm")
            .Query("maximumWaitMs", "30000").AsyncRaw();
        var acceptedTurn = source.Post("/operation/start")
            .Body(new PlayBoundedOperationReq(
                roomId,
                $"accepted-{suffix}"))
            // The operation is intentionally held until the relocation gate
            // is released. Keep this HTTP waiter open for the host deadline;
            // the request must not cancel the accepted application turn.
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<PlayBoundedOperationRes>().AsTask();
        await source.Post("/operation-gate/wait-started")
            .Query("timeoutMs", "5000").AsyncRaw();
        await source.Post("/relocate?deadlineMs=30000").AsyncRaw();

        await WaitPeerRelocatingAsync(
            target, sourceNode, TimeSpan.FromSeconds(30));
        var newlyPlaced = (await target.Post("/rooms")
            .Body(new CreateRoomReq($"room-c3-new-{suffix}"))
            .Async<CreateRoomRes>()).Body;
        ZlinkStreamAssert.Ensure(
            newlyPlaced.NodeRid != sourceNode,
            "OBS-C3 admitted new placement on a relocating host.");

        await source.Post("/operation-gate/release").AsyncRaw();
        var completedTurn = (await acceptedTurn).Body;
        ZlinkStreamAssert.Ensure(
            completedTurn.Marker == $"accepted-{suffix}"
            && completedTurn.NodeRid == sourceNode,
            "OBS-C3 did not finish the turn accepted before admission seal.");

        var relocation = await ScenarioContext.WaitForRelocationAsync(
            source, TimeSpan.FromSeconds(35));
        ZlinkStreamAssert.Ensure(
            relocation.Result == "Relocated"
            && relocation.Reason == "None"
            && relocation.TerminalCount == 1,
            $"OBS-C3 relocation returned {relocation.Result}/{relocation.Reason}.");

        var after = await WaitPlayAggregateAsync(
            target, roomId, actorId, targetNode, TimeSpan.FromSeconds(15));
        ZlinkStreamAssert.Ensure(
            after.SpotRows.Single(row => row.SpotRid == roomId).Generation
                == roomGeneration
            && after.ActorRows.Single(row => row.ActorId == actorId).Generation
                == actorGeneration,
            "OBS-C3 changed logical ObjectGeneration during relocation.");
        var action = await connector.Request(
                new GameActionReq($"obs-c3-after-{suffix}"))
            .Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(
            action.RoomRid == roomId
            && action.NodeRid == targetNode,
            "OBS-C3 aggregate did not resume service on the target owner.");

        await VerifyStaleGlobalSpotIdAsync(context, suffix);
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-C3 passed");
    }

    private static async Task VerifyStaleGlobalSpotIdAsync(
        ScenarioContext context,
        string suffix)
    {
        var workflowId = $"workflow-c3-{suffix}";
        var created = (await context.WorkflowA.Post("/workflows")
            .Body(new CreateWorkflowReq(workflowId))
            .Async<CreateWorkflowRes>()).Body;
        _ = await context.WorkflowNodeIdAsync("workflow-a");
        _ = await context.WorkflowNodeIdAsync("workflow-b");
        var owner = context.Workflow(created.NodeRid);
        var observer = context.OtherWorkflow(created.NodeRid);
        var initial = await WaitWorkflowSpotAsync(
            observer, workflowId, created.NodeRid, TimeSpan.FromSeconds(10));
        var initialGeneration = initial.SpotRows
            .Single(row => row.SpotRid == workflowId).Generation;

        await observer.Post($"/workflows/{workflowId}/advance")
            .Body(new AdvanceWorkflowReq("obs-c3-persisted"))
            .Async<AdvanceWorkflowRes>();
        await observer.Post($"/workflows/{workflowId}/stale-spot-id/capture")
            .AsyncRaw();

        await owner.Post("/shutdown?deadlineMs=10000").AsyncRaw();
        var shutdown = await ScenarioContext.WaitForShutdownAsync(
            owner, TimeSpan.FromSeconds(12));
        ZlinkStreamAssert.Ensure(
            shutdown.Result == "Stopped"
            && shutdown.Reason == "None"
            && shutdown.TerminalCount == 1,
            $"OBS-C3 workflow shutdown returned {shutdown.Result}/{shutdown.Reason}.");

        var released = await WaitWorkflowMissingAsync(
            observer, workflowId, TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(
            released.SpotRows.All(row => row.SpotRid != workflowId),
            "OBS-C3 shutdown left the Workflow Spot authority registered.");

        var stale = (await observer.Post("/stale-spot-id/execute")
            .Async<StaleSpotIdProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(
            stale.Failed && stale.ErrorKind == "NotFound",
            $"OBS-C3 stale global Spot ID returned '{stale.ErrorKind}' instead of NotFound.");
        var afterStale = (await observer.Get("/evidence")
            .Query("spotRid", workflowId)
            .Async<EvidenceSnapshot>()).Body;
        ZlinkStreamAssert.Ensure(
            afterStale.SpotRows.All(row => row.SpotRid != workflowId),
            "OBS-C3 direct request recreated a closed Spot.");

        await observer.Post("/workflows")
            .Body(new CreateWorkflowReq(workflowId)).AsyncRaw();
        var recreated = await WaitWorkflowSpotAsync(
            observer,
            workflowId,
            context.OtherWorkflowNode(created.NodeRid),
            TimeSpan.FromSeconds(10));
        var recreatedRow = recreated.SpotRows
            .Single(row => row.SpotRid == workflowId);
        ZlinkStreamAssert.Ensure(
            recreatedRow.Generation > initialGeneration,
            "OBS-C3 explicit GetOrCreate did not publish a new generation.");
        var replayed = (await observer.Get($"/workflows/{workflowId}/state")
            .Async<ReadWorkflowRes>()).Body;
        ZlinkStreamAssert.Ensure(
            replayed.Version == 1 && replayed.State == "obs-c3-persisted",
            "OBS-C3 explicit recreation did not reload application state.");
    }

    private static async Task<EvidenceSnapshot> WaitPlayAggregateAsync(
        ZLinkHttpClient observer,
        string roomId,
        string actorId,
        string ownerNode,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = (await observer.Get("/evidence")
                .Query("spotRid", roomId)
                .Query("actorId", actorId)
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.SpotRows.Any(row =>
                    row.SpotRid == roomId && row.NodeRid == ownerNode)
                && snapshot.ActorRows.Any(row =>
                    row.ActorId == actorId && row.NodeRid == ownerNode))
                return snapshot;
            await Task.Delay(100);
        }

        throw new TimeoutException(
            $"OBS-C3 aggregate owner '{ownerNode}' did not converge.");
    }

    private static async Task WaitPeerRelocatingAsync(
        ZLinkHttpClient observer,
        string sourceNode,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = (await observer.Get("/evidence")
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.PeerRows.Any(row =>
                    row.NodeRid == sourceNode && row.Draining))
                return;
            await Task.Delay(100);
        }

        throw new TimeoutException(
            $"OBS-C3 relocating marker for '{sourceNode}' did not converge.");
    }

    private static async Task<EvidenceSnapshot> WaitWorkflowSpotAsync(
        ZLinkHttpClient observer,
        string spotId,
        string ownerNode,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = (await observer.Get("/evidence")
                .Query("spotRid", spotId)
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.SpotRows.Any(row =>
                    row.SpotRid == spotId && row.NodeRid == ownerNode))
                return snapshot;
            await Task.Delay(100);
        }

        throw new TimeoutException(
            $"Workflow Spot '{spotId}' owner '{ownerNode}' did not converge.");
    }

    private static async Task<EvidenceSnapshot> WaitWorkflowMissingAsync(
        ZLinkHttpClient observer,
        string spotId,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        EvidenceSnapshot? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            last = (await observer.Get("/evidence")
                .Query("spotRid", spotId)
                .Async<EvidenceSnapshot>()).Body;
            if (last.SpotRows.All(row => row.SpotRid != spotId)) return last;
            await Task.Delay(100);
        }

        return last ?? throw new TimeoutException(
            $"Workflow Spot '{spotId}' release was not observable.");
    }
}
