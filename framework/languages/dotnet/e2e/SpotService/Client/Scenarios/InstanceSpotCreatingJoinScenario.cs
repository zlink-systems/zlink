using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class InstanceSpotCreatingJoinScenario
{
    internal static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string releaseAckFile)
    {
        var spotId = $"instance-creating-join-{Guid.NewGuid():N}";
        var first = RequestAsync(playA, spotId, "creating-first");
        var gate = await WaitForGateAsync(playA, playB, spotId, TimeSpan.FromSeconds(10));
        var creating = await WaitForLocationAsync(
            playA,
            spotId,
            "Creating",
            TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(creating.NodeRid.StartsWith(gate.Owner, StringComparison.Ordinal),
            "Creating location owner did not match the initialization owner.");

        var second = RequestAsync(playB, spotId, "creating-follow-up");
        Console.WriteLine(
            $"instance-creating-join release-ready|owner={gate.Owner}|spot={spotId}"
            + $"|generation={creating.ObjectGeneration}");
        await WaitForFileAsync(releaseAckFile, TimeSpan.FromSeconds(20));

        var replies = await Task.WhenAll(first, second);
        ZlinkStreamAssert.Ensure(replies.All(static response => response.Succeeded),
            "Creating activation requests did not both succeed.");
        ZlinkStreamAssert.Ensure(replies[0].NodeRid == replies[1].NodeRid,
            "Creating activation requests did not join the same owner.");
        var ready = await WaitForLocationAsync(playA, spotId, "Ready", TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(ready.ObjectGeneration == creating.ObjectGeneration,
            "Creating activation changed generation before Ready publication.");
        Console.WriteLine(
            $"operation InstanceSpot.creating-join passed|spot={spotId}"
            + $"|owner={gate.Owner}|generation={ready.ObjectGeneration}");
    }

    private static async Task<InstanceColdRequestRes> RequestAsync(
        ZLinkHttpClient client,
        string spotId,
        string operationId) =>
        (await client.Post("/instance/cold-request")
            .Body(new InstanceColdRequestReq(spotId, operationId))
            .Async<InstanceColdRequestRes>()).Body;

    private static async Task<(string Owner, string Line)> WaitForGateAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string spotId,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            foreach (var (owner, client) in new[] { ("play-a", playA), ("play-b", playB) })
            {
                var evidence = (await client.Get("/evidence").Async<string[]>()).Body;
                var line = evidence.FirstOrDefault(value =>
                    value.StartsWith("instance-initialize-gate|", StringComparison.Ordinal)
                    && value.Contains($"|spot={spotId}", StringComparison.Ordinal));
                if (line is not null) return (owner, line);
            }
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
        throw new TimeoutException("Instance initialization did not enter the Creating gate.");
    }

    private static async Task<InstanceLocationRes> WaitForLocationAsync(
        ZLinkHttpClient client,
        string spotId,
        string state,
        TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        InstanceLocationRes? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            last = (await client.Post("/instance/location")
                .Body(new InstanceLocationReq(spotId))
                .Async<InstanceLocationRes>()).Body;
            if (last.Found && last.State == state) return last;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }
        throw new TimeoutException(
            $"Instance Spot '{spotId}' did not reach {state}; last={last}.");
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
