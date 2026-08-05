// Verifies OBS-A4 Fanout And Timer behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsA4FanoutAndTimerScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var owner = $"workflow-owner-{suffix}";
        var subscriberA = $"projection-a-{suffix}";
        var subscriberB = $"projection-b-{suffix}";
        // Workflow(nodeRid) resolves through a cache that only fills when a
        // role is looked up, so warm both before mapping a placement result.
        _ = await context.WorkflowNodeIdAsync("workflow-a");
        _ = await context.WorkflowNodeIdAsync("workflow-b");
        // Placement picks the node for each workflow Spot, and the projection
        // evidence is written by that owner. Keep the create responses so the
        // waits below ask the hosts the Spots actually landed on.
        await context.WorkflowA.Post("/workflows").Body(new CreateWorkflowReq(owner)).AsyncRaw();
        var createdA = (await context.WorkflowA.Post("/workflows")
            .Body(new CreateWorkflowReq(subscriberA, "subscriber"))
            .Async<CreateWorkflowRes>()).Body;
        var createdB = (await context.WorkflowB.Post("/workflows")
            .Body(new CreateWorkflowReq(subscriberB, "subscriber"))
            .Async<CreateWorkflowRes>()).Body;
        await context.WorkflowA.Post($"/workflows/{owner}/advance")
            .Body(new AdvanceWorkflowReq("obs-a4-state")).AsyncRaw();
        await context.WorkflowA.Post($"/workflows/{owner}/publish")
            .Body(new PublishProjectionReq("obs-a4-fanout")).AsyncRaw();
        var expected = $"rid={owner}|version=1|marker=obs-a4-fanout";
        var receivedA = (await context.Workflow(createdA.NodeRid).Post("/evidence/wait")
            .Body(new EvidenceWaitReq([expected], [["projection-received|"]]))
            .Async<string[]>()).Body;
        var receivedB = (await context.Workflow(createdB.NodeRid).Post("/evidence/wait")
            .Body(new EvidenceWaitReq([expected], [["projection-received|"]]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(receivedA.Any(line => line.Contains($"subscriber={subscriberA}", StringComparison.Ordinal)),
            "OBS-A4 workflow-a subscriber did not receive the fanout.");
        ZlinkStreamAssert.Ensure(receivedB.Any(line => line.Contains($"subscriber={subscriberB}", StringComparison.Ordinal)),
            "OBS-A4 workflow-b subscriber did not receive the fanout.");

        var roomRid = $"timer-room-{suffix}";
        var timerRoom = (await context.PlayA.Post("/rooms")
            .Body(new CreateRoomReq(roomRid))
            .Async<CreateRoomRes>()).Body;
        var timer = await context.WaitPlayEvidenceForNodeAsync(
            timerRoom.NodeRid, $"timer-tick|room={roomRid}");
        ZlinkStreamAssert.Ensure(timer.Any(line => line.Contains($"timer-tick|room={roomRid}", StringComparison.Ordinal)),
            "OBS-A4 timer origin evidence missing.");
        Console.WriteLine("scenario OBS-A4 passed");
    }
}
