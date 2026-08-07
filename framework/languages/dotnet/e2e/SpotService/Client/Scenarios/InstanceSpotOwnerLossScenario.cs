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
            .Body(new InstanceColdRequestReq(spotId, "ready"))
            .Async<InstanceColdRequestRes>()).Body;
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

        var afterCrash = (await survivor.Post("/instance/cold-request")
            .Body(new InstanceColdRequestReq(spotId, "after-ready-crash"))
            .Async<InstanceColdRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(!afterCrash.Succeeded,
            "IS-E2E-05 request unexpectedly succeeded after Ready owner loss.");
        ZlinkStreamAssert.Ensure(
            afterCrash.ErrorKind == ZLinkFrameworkErrorKind.Unavailable.ToString(),
            $"IS-E2E-05 expected Unavailable, got {afterCrash.ErrorKind}.");
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
