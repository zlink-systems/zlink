// Verifies idle Instance Spot cleanup and a later cold activation.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class InstanceSpotIdleEvictionScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotId = $"instance-idle-{Guid.NewGuid():N}";
        var firstOperation = $"idle-first-{Guid.NewGuid():N}";
        var first = (await playA.Post("/instance/cold-request")
                .Body(new InstanceColdRequestReq(spotId, firstOperation))
                .Async<InstanceColdRequestRes>())
            .Body;
        ZlinkStreamAssert.Ensure(
            first.Succeeded,
            "Instance Spot idle scenario first request did not complete.");

        var firstEvidence = (await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                [
                    $"instance-request|rid=play-a|spot={spotId}|operation={firstOperation}",
                    $"instance-closing|rid=play-a|spot={spotId}|reason=IdleEvicted"
                ],
                TimeoutMilliseconds: 15000))
                .Async<string[]>())
            .Body;
        ZlinkStreamAssert.Ensure(
            firstEvidence.Any(line => line.Contains(
                $"instance-closing|rid=play-a|spot={spotId}|reason=IdleEvicted",
                StringComparison.Ordinal)),
            "Instance Spot was not closed with IdleEvicted.");

        var secondOperation = $"idle-second-{Guid.NewGuid():N}";
        var second = (await playA.Post("/instance/cold-request")
                .Body(new InstanceColdRequestReq(spotId, secondOperation))
                .Async<InstanceColdRequestRes>())
            .Body;
        ZlinkStreamAssert.Ensure(
            second.Succeeded,
            $"Instance Spot did not reactivate after idle cleanup: {second.ErrorKind}.");

        var evidence = (await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                [$"instance-request|rid=play-a|spot={spotId}|operation={secondOperation}"],
                TimeoutMilliseconds: 15000))
                .Async<string[]>())
            .Body;
        var initializeCount = evidence.Count(line =>
            line.Contains(
                $"instance-initialize|rid=play-a|spot={spotId}",
                StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            initializeCount >= 2,
            "Cold reactivation did not create a new Instance Spot instance.");
        Console.WriteLine("operation SpotService.instance-idle passed");
    }
}
