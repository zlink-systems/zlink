// Verifies SM-C1 Channel To Spot Messaging behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmC1ChannelToSpotMessagingScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-c1-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-C1 spot was not created on play-a.");
        var viaChannel = (await playA.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "noop", 0))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(viaChannel.SpotRid == spotRid, "SM-C1 request reached the wrong spot.");
        ZlinkStreamAssert.Ensure(viaChannel.NodeRid == "play-a", "SM-C1 request reached the wrong node.");
        var command = (await playA.Post("/spot/state/command")
            .Body(new SpotStateCommandReq(spotRid, "sm-c1-send"))
            .Async<SpotStateCommandRes>()).Body;
        ZlinkStreamAssert.Ensure(command.Accepted, "SM-C1 external channel command was not accepted.");
        var timeout = (await playA.Post("/spot/slow/request")
            .Body(new SpotSlowRouteReq(spotRid, "sm-c1-timeout", 1500, 100))
            .Async<SpotSlowRouteRes>()).Body;
        ZlinkStreamAssert.Ensure(timeout.TimedOut, "SM-C1 expected slow channel-to-spot request to time out.");
        var afterTimeout = (await playA.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "add", 1))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(afterTimeout.SpotRid == spotRid, "SM-C1 post-timeout request reached the wrong spot.");
        ZlinkStreamAssert.Ensure(afterTimeout.NodeRid == "play-a", "SM-C1 post-timeout request reached the wrong node.");
        ZlinkStreamAssert.Ensure(afterTimeout.Value > viaChannel.Value, "SM-C1 post-timeout state did not advance.");
        var expectedEvidence = new[] { $"spot-state-command|rid=play-a|spot={spotRid}|marker=sm-c1-send" };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-C1 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-c1 passed");
    }
}
