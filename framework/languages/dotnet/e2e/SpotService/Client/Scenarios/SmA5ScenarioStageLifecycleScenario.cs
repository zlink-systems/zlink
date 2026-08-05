// Verifies SM-A5 Scenario Stage Lifecycle behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies the single A5 spot-service scenario.
internal static class SmA5ScenarioStageLifecycleScenario
{
    public static async Task RunAsync(ZLinkHttpClient api)
    {
        var spotRid = $"spot-sm-a5-{Guid.NewGuid():N}";
        var created = (await api.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid, "SM-A5 did not create the requested spot.");
        ZlinkStreamAssert.Ensure(created.NodeRid == "play-a", "SM-A5 created spot on the wrong node.");

        var ready = (await api.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "noop", 0))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(ready.SpotRid == spotRid && ready.NodeRid == "play-a",
            "SM-A5 spot route did not become ready.");

        var probeReply = (await api.Post("/spot/stage/request")
            .Body(new SpotStageProbeReq(spotRid, "sm-a5-stage", 9))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(probeReply.SpotRid == spotRid, "SM-A5 stage request reached the wrong spot.");
        ZlinkStreamAssert.Ensure(probeReply.NodeRid == "play-a", "SM-A5 stage request reached the wrong node.");
        ZlinkStreamAssert.Ensure(probeReply.Value == 9, "SM-A5 stage request state mismatch.");

        var timer = (await api.Post("/spot/stage/timer")
            .Body(new SpotStageTimerReq(spotRid, "sm-a5-stage-timer", 50))
            .Async<SpotStageTimerRes>()).Body;
        ZlinkStreamAssert.Ensure(timer.SpotRid == spotRid && timer.Started, "SM-A5 stage timer was not started.");

        var closeReply = (await api.Post("/spot/close")
            .Body(new CloseSpotReq(spotRid))
            .Async<CloseSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(closeReply.Closed, "SM-A5 did not close the spot.");

        var expectedEvidence = new[]
        {
            $"spot-initialize|rid=play-a|spot={spotRid}",
            $"stage-request|rid=play-a|spot={spotRid}|marker=sm-a5-stage|value=9",
            $"stage-timer|rid=play-a|spot={spotRid}|name=sm-a5-stage-timer",
            $"spot-closing|rid=play-a|spot={spotRid}"
        };
        var evidence = (await api.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-A5 evidence mismatch.");

        Console.WriteLine("operation SpotService.sm-a5 passed");
    }
}
