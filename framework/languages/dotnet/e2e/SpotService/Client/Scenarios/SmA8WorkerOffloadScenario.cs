// Verifies SM-A8 Worker Offload behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA8WorkerOffloadScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-a8-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-A8 worker spot was not created on play-a.");
        var ready = (await playA.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "noop", 0))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(ready.SpotRid == spotRid && ready.NodeRid == "play-a",
            "SM-A8 worker spot route did not become ready.");
        var workerTask = playA.Post("/spot/worker/start")
            .Body(new SpotWorkerStartReq(spotRid, "sm-a8-worker", 5000))
            .Async<WorkerStartRes>()
            .AsTask();
        _ = await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [$"worker-start|rid=play-a|spot={spotRid}|marker=sm-a8-worker"]))
            .Async<string[]>();
        var duringWorker = (await playA.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "add", 1))
            .Async<StateRes>()).Body;
        var worker = (await workerTask).Body;
        var completed = (await playA.Post("/spot/worker/complete")
            .Body(new SpotWorkerCompleteReq(spotRid, "sm-a8-worker"))
            .Async<SpotWorkerCompleteRes>()).Body;
        ZlinkStreamAssert.Ensure(worker.SpotRid == spotRid, "SM-A8 worker start target mismatch.");
        ZlinkStreamAssert.Ensure(duringWorker.Value == 1,
            "SM-A8 concurrent spot request did not run before worker completion.");
        ZlinkStreamAssert.Ensure(completed.Completed, "SM-A8 worker did not complete.");
        var workerStartIndex = Array.FindIndex(completed.Evidence,
            line => line.Contains($"worker-start|rid=play-a|spot={spotRid}|marker=sm-a8-worker", StringComparison.Ordinal));
        var stateIndex = Array.FindIndex(completed.Evidence,
            line => line.Contains($"spot-state-request|rid=play-a|spot={spotRid}|value=1", StringComparison.Ordinal));
        var workerCompleteIndex = Array.FindIndex(completed.Evidence,
            line => line.Contains($"worker-complete|rid=play-a|spot={spotRid}|marker=sm-a8-worker", StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            workerStartIndex >= 0 && stateIndex > workerStartIndex && workerCompleteIndex > stateIndex,
            "SM-A8 expected spot request evidence between worker start and completion.");
        Console.WriteLine("operation SpotService.sm-a8 passed");
    }
}
