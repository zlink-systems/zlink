// Verifies SM-F3 Mixed Route And Spot Egress behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmF3MixedRouteAndSpotEgressScenario
{
    public static async Task<string> RunAsync(ZLinkHttpClient playA, ZLinkHttpClient gateway)
    {
        var spotRid = $"sm-f3-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid, "SM-F3 target spot was not created.");

        var before = await RoutePingAsync(gateway, "sm-f3-before");
        ZlinkStreamAssert.Ensure(
            before is { Value: "sm-f3-before", NodeRid: "play-a" },
            "SM-F3 ordinary route request did not use the shared route channel.");

        var state = (await gateway.Post("/spot/route-state")
            .Body(new SpotStateRouteReq(spotRid, "add", 3))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(
            state is { SpotRid: var actualSpotRid, NodeRid: "play-a", Value: 3 }
            && actualSpotRid == spotRid,
            "SM-F3 spot route request was not dispatched to its target spot.");

        var afterMixed = await RoutePingAsync(gateway, "sm-f3-after");
        ZlinkStreamAssert.Ensure(
            afterMixed is { Value: "sm-f3-after", NodeRid: "play-a" },
            "SM-F3 spot routing interfered with ordinary route messaging.");

        var expectedEvidence = new[]
        {
            "control-ping|rid=play-a|value=sm-f3-before",
            $"spot-state-request|rid=play-a|spot={spotRid}|value=3",
            "control-ping|rid=play-a|value=sm-f3-after"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line =>
                line.Contains(expected, StringComparison.Ordinal))),
            "SM-F3 evidence mismatch.");
        Console.WriteLine("SM-F3 PASS");
        return spotRid;
    }

    private static async Task<ControlPingRes> RoutePingAsync(
        ZLinkHttpClient gateway,
        string marker)
    {
        return (await gateway.Post("/channel/route-ping")
            .Body(new ControlPingReq(marker))
            .Async<ControlPingRes>()).Body;
    }
}
