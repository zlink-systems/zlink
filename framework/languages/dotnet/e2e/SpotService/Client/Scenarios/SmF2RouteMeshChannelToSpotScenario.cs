// Verifies SM-F2 Route Mesh Channel To Spot behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmF2RouteMeshChannelToSpotScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient source,
        ZLinkHttpClient target)
    {
        var spotRid = $"spot-sm-f2-{Guid.NewGuid():N}";
        try
        {
            // Force placement onto the remote Object Server. The request
            // still originates at source and carries only the global Spot ID.
            await source.Post("/placement-weight")
                .Body(new PlacementWeightReq(0))
                .Async<PlacementWeightRes>();
            await target.Post("/placement-weight")
                .Body(new PlacementWeightReq(100))
                .Async<PlacementWeightRes>();
            await source.Post("/spot/create")
                .Body(new CreateSpotReq(spotRid))
                .Async<CreateSpotRes>();

            var state = (await source.Post("/spot/state/request")
                .Body(new SpotStateRouteReq(spotRid, "add", 5))
                .Async<StateRes>()).Body;
            ZlinkStreamAssert.Ensure(
                state.SpotRid == spotRid,
                "SM-F2 request reached the wrong spot.");
            ZlinkStreamAssert.Ensure(
                state.NodeRid == "play-b"
                || state.NodeRid.StartsWith(
                    "play-b-",
                    StringComparison.Ordinal),
                "SM-F2 request did not reach the remote owner.");
            ZlinkStreamAssert.Ensure(
                state.Value == 5,
                "SM-F2 state reply mismatch.");

            var command = (await source.Post("/spot/state/command")
                .Body(new SpotStateCommandReq(spotRid, "sm-f2-command"))
                .Async<SpotStateCommandRes>()).Body;
            ZlinkStreamAssert.Ensure(
                command.SpotRid == spotRid && command.Accepted,
                "SM-F2 command was not accepted.");
            var expectedEvidence = new[]
            {
                $"spot-state-request|rid=play-b|spot={spotRid}|value=5",
                $"spot-state-command|rid=play-b|spot={spotRid}|marker=sm-f2-command"
            };
            var targetEvidence = (await target.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(expectedEvidence))
                .Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                expectedEvidence.All(expected => targetEvidence.Any(line =>
                    line.Contains(expected, StringComparison.Ordinal))),
                "SM-F2 remote owner evidence mismatch.");

            var sourceEvidence =
                (await source.Get("/evidence").Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                sourceEvidence.All(line =>
                    !line.Contains($"spot={spotRid}", StringComparison.Ordinal)
                    || !line.Contains("spot-state-", StringComparison.Ordinal)),
                "SM-F2 source node handled the remote Spot operation.");
            Console.WriteLine("operation SpotService.sm-f2 passed");
        }
        finally
        {
            await source.Post("/placement-weight")
                .Body(new PlacementWeightReq(100))
                .Async<PlacementWeightRes>();
            await target.Post("/placement-weight")
                .Body(new PlacementWeightReq(100))
                .Async<PlacementWeightRes>();
        }
    }
}
