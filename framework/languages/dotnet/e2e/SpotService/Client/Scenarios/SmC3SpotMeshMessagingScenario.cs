// Verifies SM-C3 Spot Mesh Messaging behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies direct spot-to-spot request, send, publish, timeout, and missing handler behavior.
internal static class SmC3SpotMeshMessagingScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var sourceSpotRid = $"spot-sm-c3-source-{Guid.NewGuid():N}";
        var targetSpotRid = $"spot-sm-c3-target-{Guid.NewGuid():N}";
        foreach (var spotRid in new[] { sourceSpotRid, targetSpotRid })
        {
            var created = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(spotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
                "SM-C3 spot was not created on play-a.");
        }

        var direct = (await playA.Post("/spot/to-spot/request")
            .Body(new SpotToSpotRouteReq(sourceSpotRid, targetSpotRid, "direct"))
            .Async<SpotToSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(direct.SourceSpotRid == sourceSpotRid, "SM-C3 source spot mismatch.");
        ZlinkStreamAssert.Ensure(direct.TargetSpotRid == targetSpotRid, "SM-C3 target spot mismatch.");
        ZlinkStreamAssert.Ensure(direct.TargetValue >= 3, "SM-C3 target state was not updated.");

        var timeout = (await playA.Post("/spot/to-spot/timeout")
            .Body(new SpotToSpotTimeoutRouteReq(sourceSpotRid, targetSpotRid, "slow"))
            .Async<SpotToSpotTimeoutRes>()).Body;
        ZlinkStreamAssert.Ensure(timeout.Failed, "SM-C3 slow target request did not time out.");

        var negative = (await playA.Post("/spot/to-spot/negative")
            .Body(new SpotToSpotNegativeRouteReq(sourceSpotRid, targetSpotRid, "missing"))
            .Async<SpotToSpotNegativeRes>()).Body;
        ZlinkStreamAssert.Ensure(negative.RequestFailed, "SM-C3 missing target handler request did not fail.");

        var expectedEvidence = new[]
        {
            $"spot-to-spot|rid=play-a|source={sourceSpotRid}|target={targetSpotRid}|value=",
            $"spot-state-command|rid=play-a|spot={targetSpotRid}|marker=sm-c3-send-direct",
            $"spot-msg|rid=play-a|spot={targetSpotRid}|marker=sm-c3-publish-direct",
            $"spot-to-spot-timeout|rid=play-a|source={sourceSpotRid}|target={targetSpotRid}|failed=True",
            $"spot-to-spot-negative|rid=play-a|source={sourceSpotRid}|target={targetSpotRid}|requestFailed=True",
            "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=ReplyError|packet=MissingSpotReq",
            "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=Drop|packet=MissingSpotMsg"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-C3 evidence mismatch.");

        Console.WriteLine("operation SpotService.sm-c3 passed");
    }
}
