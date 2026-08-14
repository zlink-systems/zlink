// Verifies SM-C6 Logical Multicast ROUTER backpressure behavior.
using System.Diagnostics;
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmC6LogicalMulticastBackpressureScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string pauseAckFile,
        string resumeAckFile)
    {
        var playASpot = $"spot-sm-c6-a-{Guid.NewGuid():N}";
        var playBSpot = $"spot-sm-c6-b-{Guid.NewGuid():N}";
        await SetPlacementWeightsAsync(playA, playB, 100, 0);
        try
        {
            var createdA = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(playASpot))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(createdA.NodeRid == "play-a",
                "SM-C6 source Spot was not created on play-a.");

            await SetPlacementWeightsAsync(playA, playB, 0, 100);
            var createdB = (await playB.Post("/spot/create")
                .Body(new CreateSpotReq(playBSpot))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(createdB.NodeRid == "play-b",
                "SM-C6 blocked Spot was not created on play-b.");

            var blockerMarker = $"sm-c6-blocker-{Guid.NewGuid():N}";
            var blocker = (await gateway.Post("/spot/backpressure-publish")
                .Body(new SpotBackpressurePublishReq(
                    blockerMarker,
                    PayloadBytes: 1024 * 1024,
                    MaxAttempts: 16,
                    GateSpotId: playBSpot))
                .Async<SpotBackpressurePublishRes>()).Body;
            ZlinkStreamAssert.Ensure(blocker.AcceptedCount == 16,
                $"SM-C6 blocker publish accepted {blocker.AcceptedCount} messages, expected 16.");
            await WaitForEvidenceAsync(
                playB,
                $"spot-backpressure-entered|rid=play-b|spot={playBSpot}|marker={blockerMarker}");

            Console.WriteLine("spot-service sm-c6 pause-play-b-ready");
            await WaitForRunnerAckAsync(
                pauseAckFile,
                "the paused target");

            var marker = $"sm-c6-{Guid.NewGuid():N}";
            var publish = (await gateway.Post("/spot/backpressure-publish")
                .Body(new SpotBackpressurePublishReq(
                    marker,
                    PayloadBytes: 1024 * 1024,
                    MaxAttempts: 1))
                .Async<SpotBackpressurePublishRes>()).Body;
            ZlinkStreamAssert.Ensure(publish.AcceptedCount == 1,
                "SM-C6 marker publish did not complete through the public terminal.");
            await WaitForEvidenceAsync(
                playA,
                $"spot-backpressure|rid=play-a|spot={playASpot}|marker={marker}|sequence={publish.StartSequence}");

            await VerifyPausedTargetDidNotReceiveAsync(
                playA,
                playB,
                Evidence("play-a", playASpot, marker, publish.StartSequence),
                Evidence("play-b", playBSpot, marker, publish.StartSequence));

            Console.WriteLine("spot-service sm-c6 resume-play-b-ready");
            await WaitForRunnerAckAsync(
                resumeAckFile,
                "the resumed target");
            await WaitForEvidenceAsync(
                playB,
                $"spot-backpressure|rid=play-b|spot={playBSpot}|marker={marker}|sequence={publish.StartSequence}");
            await VerifySettledDeliveryAsync(
                playA,
                playB,
                Evidence("play-a", playASpot, marker, publish.StartSequence),
                Evidence("play-b", playBSpot, marker, publish.StartSequence));

            Console.WriteLine($"operation SpotService.sm-c6 passed accepted={publish.AcceptedCount}");
        }
        finally
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 100);
        }
    }

    static async Task WaitForRunnerAckAsync(string ackFile, string operation)
    {
        ZlinkStreamAssert.Ensure(
            !string.IsNullOrWhiteSpace(ackFile),
            $"SM-C6 runner did not provide the acknowledgement file for {operation}.");
        // Local runner orchestration is part of the readiness contract. A
        // delayed acknowledgement must remain visible instead of being hidden
        // by a long fallback deadline.
        var elapsed = Stopwatch.StartNew();
        while (!File.Exists(ackFile))
        {
            ZlinkStreamAssert.Ensure(
                elapsed.Elapsed < TimeSpan.FromSeconds(3),
                $"SM-C6 runner did not acknowledge {operation}.");
            await Task.Delay(TimeSpan.FromMilliseconds(5));
        }
    }

    static async Task VerifyPausedTargetDidNotReceiveAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string playAEvidence,
        string playBEvidence)
    {
        var readyEvidence = (await playA.Get("/evidence").Async<string[]>()).Body;
        var blockedEvidence = (await playB.Get("/evidence").Async<string[]>()).Body;
        var deliveredToA = readyEvidence.Count(line => line.Contains(playAEvidence, StringComparison.Ordinal));
        var deliveredToB = blockedEvidence.Count(line => line.Contains(playBEvidence, StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            deliveredToA == 1 && deliveredToB == 0,
            "SM-C6 ready target did not receive exactly one marker before the paused target resumed.");
    }

    static async Task VerifySettledDeliveryAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string playAEvidence,
        string playBEvidence)
    {
        var settledA = (await playA.Get("/evidence").Async<string[]>()).Body;
        var settledB = (await playB.Get("/evidence").Async<string[]>()).Body;
        var deliveredToA = settledA.Count(line => line.Contains(playAEvidence, StringComparison.Ordinal));
        var deliveredToB = settledB.Count(line => line.Contains(playBEvidence, StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            deliveredToA == 1 && deliveredToB == 1,
            "SM-C6 marker delivery was not exactly once at both targets after the paused target resumed.");
    }

    static async Task WaitForEvidenceAsync(ZLinkHttpClient client, string expected)
    {
        var evidence = (await client.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([expected]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains(expected, StringComparison.Ordinal)),
            $"SM-C6 evidence did not contain '{expected}'.");
    }

    static string Evidence(string nodeRid, string spotId, string marker, int sequence)
        => $"spot-backpressure|rid={nodeRid}|spot={spotId}"
           + $"|marker={marker}|sequence={sequence}";

    static async Task SetPlacementWeightsAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        int playAWeight,
        int playBWeight)
    {
        await playA.Post("/placement-weight")
            .Body(new PlacementWeightReq(playAWeight))
            .Async<PlacementWeightRes>();
        await playB.Post("/placement-weight")
            .Body(new PlacementWeightReq(playBWeight))
            .Async<PlacementWeightRes>();
        await Task.Delay(TimeSpan.FromSeconds(2));
    }
}
