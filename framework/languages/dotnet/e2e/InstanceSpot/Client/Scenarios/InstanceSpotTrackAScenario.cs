// Verifies cold activation, accepted send, and concurrent first-request ownership for Instance Spot.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class InstanceSpotTrackAScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        await RunColdRequestAsync(playA, playB);
        await RunColdSendAsync(playA, playB);
        await RunConcurrentFirstRequestAsync(playA, playB);
        Console.WriteLine("operation InstanceSpot.track-a passed");
    }

    private static async Task RunColdRequestAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var spotId = $"instance-is-e2e-01-{Guid.NewGuid():N}";
        var operationId = "cold-request";
        var result = (await playA.Post("/instance/cold-request")
            .Body(new InstanceColdRequestReq(spotId, operationId))
            .Async<InstanceColdRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(result.Succeeded, "IS-E2E-01 cold request did not complete.");
        ZlinkStreamAssert.Ensure(result.SpotId == spotId && result.OperationId == operationId,
            "IS-E2E-01 reply identity did not match the request.");
        await WaitForEvidenceAsync(
            playA,
            playB,
            spotId,
            operationId,
            "instance-request");
        var evidence = await ReadEvidenceAsync(playA, playB);
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-initialize", spotId) == 1,
            "IS-E2E-01 expected exactly one Instance factory initialization.");
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-request", spotId, operationId) == 1,
            "IS-E2E-01 expected exactly one request handler execution.");
    }

    private static async Task RunColdSendAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var spotId = $"instance-is-e2e-02-{Guid.NewGuid():N}";
        var operationId = "cold-send";
        var result = (await playB.Post("/instance/cold-send")
            .Body(new InstanceColdSendReq(spotId, operationId))
            .Async<InstanceColdSendRes>()).Body;
        ZlinkStreamAssert.Ensure(result.Accepted, "IS-E2E-02 cold send was not accepted.");
        await WaitForEvidenceAsync(
            playA,
            playB,
            spotId,
            operationId,
            "instance-send");
        var evidence = await ReadEvidenceAsync(playA, playB);
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-initialize", spotId) == 1,
            "IS-E2E-02 expected exactly one Instance factory initialization.");
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-send", spotId, operationId) == 1,
            "IS-E2E-02 expected exactly one send handler execution.");
    }

    private static async Task RunConcurrentFirstRequestAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var spotId = $"instance-is-e2e-03-{Guid.NewGuid():N}";
        var requests = new[]
        {
            playA.Post("/instance/cold-request")
                .Body(new InstanceColdRequestReq(spotId, "concurrent-a"))
                .Async<InstanceColdRequestRes>()
                .AsTask(),
            playB.Post("/instance/cold-request")
                .Body(new InstanceColdRequestReq(spotId, "concurrent-b"))
                .Async<InstanceColdRequestRes>()
                .AsTask()
        };
        var results = (await Task.WhenAll(requests)).Select(static response => response.Body).ToArray();
        ZlinkStreamAssert.Ensure(results.All(static result => result.Succeeded),
            "IS-E2E-03 concurrent first requests did not all complete.");
        ZlinkStreamAssert.Ensure(results.Select(static result => result.NodeRid).Distinct(StringComparer.Ordinal).Count() == 1,
            "IS-E2E-03 requests did not converge on one owner.");
        await WaitForEvidenceAsync(
            playA,
            playB,
            spotId,
            "concurrent-b",
            "instance-request");
        var evidence = await ReadEvidenceAsync(playA, playB);
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-initialize", spotId) == 1,
            "IS-E2E-03 expected one factory initialization for concurrent calls.");
        ZlinkStreamAssert.Ensure(CountForSpot(evidence, "instance-request", spotId) == 2,
            "IS-E2E-03 expected one handler execution per operation.");
    }

    private static async Task WaitForEvidenceAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string spotId,
        string operationId,
        string kind)
    {
        var expected = new[]
        {
            $"{kind}|",
            $"|spot={spotId}",
            $"|operation={operationId}"
        };
        var response = await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expected))
            .Async<string[]>();
        if (!response.Body.Any(line => expected.All(line.Contains)))
        {
            response = await playB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(expected))
                .Async<string[]>();
        }

        ZlinkStreamAssert.Ensure(response.Body.Any(line => expected.All(line.Contains)),
            $"Evidence did not arrive for {kind} and operation {operationId}.");
    }

    private static async Task<string[]> ReadEvidenceAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var first = (await playA.Get("/evidence").Async<string[]>()).Body;
        var second = (await playB.Get("/evidence").Async<string[]>()).Body;
        return first.Concat(second).ToArray();
    }

    private static int CountForSpot(
        IEnumerable<string> evidence,
        string kind,
        string spotId,
        string? operationId = null)
    {
        var spotMarker = $"|spot={spotId}";
        var operationMarker = operationId is null ? null : $"|operation={operationId}";
        return evidence.Count(line =>
            line.StartsWith(kind + "|", StringComparison.Ordinal)
            && line.Contains(spotMarker, StringComparison.Ordinal)
            && (operationMarker is null || line.Contains(operationMarker, StringComparison.Ordinal)));
    }
}
