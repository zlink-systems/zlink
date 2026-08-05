// Verifies SM-C2 Spot To Channel Messaging behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmC2SpotToChannelMessagingScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-c2-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-C2 spot was not created on play-a.");
        var outbound = (await playA.Post("/spot/outbound")
            .Body(new SpotOutboundRouteReq(spotRid, "sm-c2"))
            .Async<SpotOutboundRouteRes>()).Body;
        ZlinkStreamAssert.Ensure(outbound.Accepted, "SM-C2 outbound route was not accepted.");
        var negative = (await playA.Post("/spot/outbound-negative")
            .Body(new SpotOutboundRouteReq(spotRid, "sm-c2-missing"))
            .Async<SpotOutboundRouteRes>()).Body;
        ZlinkStreamAssert.Ensure(negative.Accepted, "SM-C2 negative outbound route was not accepted.");
        var expectedEvidence = new[]
        {
            $"spot-outbound|rid=play-a|spot={spotRid}|echo=echo-sm-c2|notify=notify-sm-c2",
            $"spot-msg|rid=play-a|spot={spotRid}|marker=sm-c2-publish",
            $"spot-outbound-negative|rid=play-a|spot={spotRid}|requestFailed=True",
            "channel-echo|value=sm-c2",
            "channel-notify|marker=notify-sm-c2",
            "dispatch-error|surface=Channel|reason=HandlerMissing|action=ReplyError|packet=MissingChannelReq",
            "dispatch-error|surface=Channel|reason=HandlerMissing|action=Drop|packet=MissingChannelNotify"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-C2 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-c2 passed");
    }
}
