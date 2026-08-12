// Verifies queued instance Spot requests fail consistently when the owner is lost.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class InstanceSpotQueueOwnerLossScenario
{
    internal static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string crashAckFile,
        string restartAckFile)
    {
        var spotId = $"instance-is-e2e-35-{Guid.NewGuid():N}";
        var initial = (await playA.Post("/instance/cold-request")
            .Body(new InstanceColdProbeReq(spotId, "ready"))
            .Async<InstanceColdProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(initial.Succeeded, "IS-E2E-35 initial request failed.");
        var owner = OwnerRole(initial.NodeRid);
        var ownerClient = owner == "play-a" ? playA : playB;
        var survivor = owner == "play-a" ? playB : playA;
        var ready = await WaitForLocationAsync(
            ownerClient,
            spotId,
            "Ready",
            TimeSpan.FromSeconds(10));

        var first = RequestAsync(survivor, spotId, "queued-first");
        var followUp = RequestAsync(survivor, spotId, "queued-follow-up");
        await WaitForEvidenceAsync(
            ownerClient,
            spotId,
            "queued-first",
            TimeSpan.FromSeconds(10));
        Console.WriteLine(
            $"instance-queue-owner-loss crash-ready|owner={owner}|spot={spotId}"
            + $"|generation={ready.ObjectGeneration}");
        await WaitForFileAsync(crashAckFile, TimeSpan.FromSeconds(20));

        var terminals = await Task.WhenAll(first, followUp);
        ZlinkStreamAssert.Ensure(terminals.All(static result =>
                !result.Succeeded && result.ErrorKind.Length != 0),
            "IS-E2E-35 queued requests did not each receive a failure terminal.");
        Console.WriteLine(
            $"instance-queue-owner-loss restart-ready|owner={owner}|spot={spotId}");
        await WaitForFileAsync(restartAckFile, TimeSpan.FromSeconds(30));

        var unavailable = await WaitForLocationAsync(
            survivor,
            spotId,
            "Unavailable",
            TimeSpan.FromSeconds(20));
        ZlinkStreamAssert.Ensure(unavailable.ObjectGeneration == ready.ObjectGeneration,
            "IS-E2E-35 restart changed the object generation.");
        var afterRestart = await RequestAsync(survivor, spotId, "after-owner-restart");
        ZlinkStreamAssert.Ensure(
            !afterRestart.Succeeded
            && afterRestart.ErrorKind == ZLinkFrameworkErrorKind.Unavailable.ToString(),
            $"IS-E2E-35 expected Unavailable after restart, got {afterRestart.ErrorKind}.");
        Console.WriteLine(
            $"operation InstanceSpot.queue-owner-loss passed|spot={spotId}"
            + $"|first={terminals[0].ErrorKind}|followUp={terminals[1].ErrorKind}");
    }

    private static async Task<InstanceColdProbeRes> RequestAsync(
        ZLinkHttpClient client,
        string spotId,
        string operationId) =>
        (await client.Post("/instance/cold-request")
            .Body(new InstanceColdProbeReq(spotId, operationId))
            .Async<InstanceColdProbeRes>()).Body;

    private static string OwnerRole(string nodeRid)
    {
        if (nodeRid.StartsWith("play-a", StringComparison.Ordinal)) return "play-a";
        if (nodeRid.StartsWith("play-b", StringComparison.Ordinal)) return "play-b";
        throw new InvalidOperationException(
            $"IS-E2E-35 did not report an exact owner: {nodeRid}.");
    }

    private static async Task WaitForEvidenceAsync(
        ZLinkHttpClient client,
        string spotId,
        string operationId,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var evidence = (await client.Get("/evidence").Async<string[]>()).Body;
            if (evidence.Any(line =>
                    line.StartsWith("instance-handler-gate|", StringComparison.Ordinal)
                    && line.Contains($"|spot={spotId}", StringComparison.Ordinal)
                    && line.Contains($"|operation={operationId}", StringComparison.Ordinal)))
                return;
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
        throw new TimeoutException("IS-E2E-35 first handler did not enter the gate.");
    }

    private static async Task<InstanceLocationRes> WaitForLocationAsync(
        ZLinkHttpClient client,
        string spotId,
        string expectedState,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        InstanceLocationRes? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            last = (await client.Post("/instance/location")
                .Body(new InstanceLocationReq(spotId))
                .Async<InstanceLocationRes>()).Body;
            if (last.Found && last.State == expectedState) return last;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }
        throw new TimeoutException(
            $"Instance Spot '{spotId}' did not reach {expectedState}; last={last}.");
    }

    private static async Task WaitForFileAsync(string path, TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            if (File.Exists(path)) return;
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
        throw new TimeoutException($"Timed out waiting for runner acknowledgement '{path}'.");
    }
}
