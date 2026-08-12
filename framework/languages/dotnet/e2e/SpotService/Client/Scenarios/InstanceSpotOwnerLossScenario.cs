// Verifies a Ready instance Spot stays unavailable after its owner is lost.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class InstanceSpotOwnerLossScenario
{
    internal static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string crashAckFile)
    {
        var spotId = $"instance-is-e2e-05-{Guid.NewGuid():N}";
        var initial = (await playA.Post("/instance/cold-request")
            .Body(new InstanceColdProbeReq(spotId, "ready"))
            .Async<InstanceColdProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(initial.Succeeded, "IS-E2E-05 initial request failed.");
        var owner = initial.NodeRid.StartsWith("play-a", StringComparison.Ordinal)
            ? "play-a"
            : initial.NodeRid.StartsWith("play-b", StringComparison.Ordinal)
                ? "play-b"
                : string.Empty;
        ZlinkStreamAssert.Ensure(owner.Length != 0,
            "IS-E2E-05 did not report an exact owner.");

        var ready = await WaitForLocationAsync(
            playA,
            spotId,
            "Ready",
            TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(ready.NodeRid == initial.NodeRid,
            "IS-E2E-05 public Ready owner did not match the handler owner.");
        Console.WriteLine(
            $"instance-owner-loss crash-ready|owner={owner}|spot={spotId}"
            + $"|generation={ready.ObjectGeneration}");

        await WaitForFileAsync(crashAckFile, TimeSpan.FromSeconds(20));
        var survivor = owner == "play-a" ? playB : playA;
        var unavailable = await WaitForLocationAsync(
            survivor,
            spotId,
            "Unavailable",
            TimeSpan.FromSeconds(20));
        ZlinkStreamAssert.Ensure(unavailable.ObjectGeneration == ready.ObjectGeneration,
            "IS-E2E-05 owner loss changed the object generation.");

        using var boundedFailure = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var requests = Enumerable.Range(0, 32).Select(async index =>
            (await survivor.Post("/instance/cold-request")
                .Body(new InstanceColdProbeReq(spotId, $"after-ready-crash-{index}"))
                .Async<InstanceColdProbeRes>(boundedFailure.Token)).Body);
        var afterCrash = await Task.WhenAll(requests);
        ZlinkStreamAssert.Ensure(
            afterCrash.Length == 32 && afterCrash.All(result => !result.Succeeded),
            "IS-E2E-05 concurrent request unexpectedly succeeded after Ready owner loss.");
        ZlinkStreamAssert.Ensure(
            afterCrash.All(result =>
                result.ErrorKind == ZLinkFrameworkErrorKind.Unavailable.ToString()),
            $"IS-E2E-05 expected only Unavailable terminals, got"
            + $" [{string.Join(", ", afterCrash.Select(result => result.ErrorKind).Distinct())}].");
        Console.WriteLine(
            $"operation InstanceSpot.owner-loss passed|spot={spotId}"
            + $"|generation={ready.ObjectGeneration}");
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
            if (last.Found && last.State == expectedState)
                return last;
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
