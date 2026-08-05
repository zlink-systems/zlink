// Verifies SM-F5 Closed Spot Route Isolation behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmF5ClosedSpotRouteIsolationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient gateway)
    {
        var spotRid = $"spot-sm-f5-{Guid.NewGuid():N}";
        await playA.Post("/spot/create").Body(new CreateSpotReq(spotRid)).Async<CreateSpotRes>();
        var closed = (await playA.Post("/spot/close")
            .Body(new CloseSpotReq(spotRid))
            .Async<CloseSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(closed.Closed, "SM-F5 target spot did not close.");

        try
        {
            await gateway.Post("/spot/route-state")
                .Body(new SpotStateRouteReq(spotRid, "add", 1))
                .Async<StateRes>();
            throw new InvalidOperationException(
                "SM-F5 closed target spot still accepted a routed request.");
        }
        catch (ZLinkFrameworkException error) when (
            error.Kind == ZLinkFrameworkErrorKind.InternalFailure)
        {
        }

        var afterClose = (await gateway.Post("/channel/route-ping")
            .Body(new ControlPingReq("sm-f5-after-close"))
            .Async<ControlPingRes>()).Body;
        ZlinkStreamAssert.Ensure(
            afterClose is { Value: "sm-f5-after-close", NodeRid: "play-a" },
            "SM-F5 closing the routed spot stopped ordinary route messaging.");

        var expectedEvidence = new[]
        {
            $"spot-closing|rid=play-a|spot={spotRid}",
            "control-ping|rid=play-a|value=sm-f5-after-close"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line =>
                line.Contains(expected, StringComparison.Ordinal))),
            "SM-F5 evidence mismatch.");
        Console.WriteLine("SM-F5 PASS");
    }
}
