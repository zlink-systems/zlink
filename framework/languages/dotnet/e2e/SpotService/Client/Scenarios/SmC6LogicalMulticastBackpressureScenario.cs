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
        string resumeAckFile,
        string blockingPauseAckFile)
    {
        var playASpot = $"spot-sm-c6-a-{Guid.NewGuid():N}";
        var playBSpot = $"spot-sm-c6-b-{Guid.NewGuid():N}";
        await playA.Post("/spot/create")
            .Body(new CreateSpotReq(playASpot))
            .Async<CreateSpotRes>();
        await playB.Post("/spot/create")
            .Body(new CreateSpotReq(playBSpot))
            .Async<CreateSpotRes>();

        // The runner starts both manually pinned peers before the client. The
        // publish detail below is the authoritative readiness check: it must
        // snapshot exactly those two remote targets, so no fixed settle delay
        // is needed here.

        Console.WriteLine("spot-service sm-c6 pause-play-b-ready");
        await WaitForRunnerAckAsync(
            pauseAckFile,
            "the first paused target");

        var marker = $"sm-c6-{Guid.NewGuid():N}";
        var selectedNonBlocking = (await gateway.Post("/spot/backpressure-publish")
            // A large frame fills the stopped peer's kernel send buffer
            // deterministically. Small frames can all leave the ROUTER pipe
            // before its HWM is observable, which tests host socket capacity
            // instead of non-blocking per-target admission.
            .Body(new SpotBackpressurePublishReq(
                marker,
                PayloadBytes: 1024 * 1024,
                MaxAttempts: 200))
            .Async<SpotBackpressureAttemptRes>()).Body;
        EnsurePartialAdmission(selectedNonBlocking, "non-blocking");

        ZlinkStreamAssert.Ensure(
            selectedNonBlocking.Status == "Backpressured",
            $"SM-C6 non-blocking status was {selectedNonBlocking.Status}, expected Backpressured.");
        ZlinkStreamAssert.Ensure(
            selectedNonBlocking.ElapsedMilliseconds < 1000,
            $"SM-C6 non-blocking submit took {selectedNonBlocking.ElapsedMilliseconds}ms.");

        // Restore write credit before testing the blocking path. Reusing a
        // pipe immediately after a DONTWAIT rejection tests recovery rather
        // than the blocking timeout contract itself.
        Console.WriteLine("spot-service sm-c6 resume-play-b-between-modes-ready");
        await WaitForRunnerAckAsync(
            resumeAckFile,
            "the resumed target");
        await Task.Delay(TimeSpan.FromSeconds(2));
        Console.WriteLine("spot-service sm-c6 pause-play-b-blocking-ready");
        await WaitForRunnerAckAsync(
            blockingPauseAckFile,
            "the second paused target");

        var blocking = (await gateway.Post("/spot/backpressure-publish")
            .Body(new SpotBackpressurePublishReq(
                marker,
                PayloadBytes: 1024 * 1024,
                MaxAttempts: 200,
                Blocking: true,
                StartSequence: selectedNonBlocking.Sequence + 1))
            .Async<SpotBackpressureAttemptRes>()).Body;
        EnsurePartialAdmission(blocking, "blocking");
        ZlinkStreamAssert.Ensure(
            blocking.Status == "Backpressured",
            $"SM-C6 blocking status was {blocking.Status}, expected Backpressured.");
        ZlinkStreamAssert.Ensure(
            blocking.ElapsedMilliseconds >= 100
            && blocking.ElapsedMilliseconds <= 5000,
            $"SM-C6 blocking submit did not respect the configured timeout: "
            + $"{blocking.ElapsedMilliseconds}ms.");

        Console.WriteLine("spot-service sm-c6 resume-play-b-ready");
        await VerifyExactlyOneDeliveryAsync(
            playA,
            playB,
            Evidence(playASpot, marker, selectedNonBlocking.Sequence),
            Evidence(playBSpot, marker, selectedNonBlocking.Sequence),
            "non-blocking");
        await VerifyExactlyOneDeliveryAsync(
            playA,
            playB,
            Evidence(playASpot, marker, blocking.Sequence),
            Evidence(playBSpot, marker, blocking.Sequence),
            "blocking");

        Console.WriteLine(
            $"operation SpotService.sm-c6 passed non_blocking_ms={selectedNonBlocking.ElapsedMilliseconds}"
            + $" blocking_ms={blocking.ElapsedMilliseconds}");
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

    static void EnsurePartialAdmission(SpotBackpressureAttemptRes attempt, string operation)
    {
        ZlinkStreamAssert.Ensure(
            attempt.SnapshotRemoteNodeCount >= 2,
            $"SM-C6 {operation} remote snapshot was {attempt.SnapshotRemoteNodeCount}, expected at least 2.");
        ZlinkStreamAssert.Ensure(
            attempt.AdmittedRemoteNodeCount + 1 == attempt.SnapshotRemoteNodeCount,
            $"SM-C6 {operation} admitted count was {attempt.AdmittedRemoteNodeCount} for snapshot "
            + $"{attempt.SnapshotRemoteNodeCount}; exactly one stopped target must be excluded.");
        ZlinkStreamAssert.Ensure(
            attempt.DroppedRemoteNodeCount == 1,
            $"SM-C6 {operation} dropped count was {attempt.DroppedRemoteNodeCount}, expected 1.");
        ZlinkStreamAssert.Ensure(
            attempt.SnapshotLocalSpotCount == 0
            && attempt.AdmittedLocalSpotCount == 0
            && attempt.DroppedLocalSpotCount == 0,
            $"SM-C6 {operation} unexpectedly included local Spot targets.");
    }

    static async Task VerifyExactlyOneDeliveryAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string playAEvidence,
        string playBEvidence,
        string operation)
    {
        var elapsed = Stopwatch.StartNew();
        bool deliveredToA;
        bool deliveredToB;
        do
        {
            var a = (await playA.Get("/evidence").Async<string[]>()).Body;
            var b = (await playB.Get("/evidence").Async<string[]>()).Body;
            deliveredToA = a.Any(line => line.Contains(playAEvidence, StringComparison.Ordinal));
            deliveredToB = b.Any(line => line.Contains(playBEvidence, StringComparison.Ordinal));
            if (deliveredToA ^ deliveredToB)
                break;
            ZlinkStreamAssert.Ensure(
                elapsed.Elapsed < TimeSpan.FromSeconds(3),
                $"SM-C6 {operation} multicast was not delivered to exactly one target.");
            await Task.Delay(TimeSpan.FromMilliseconds(25));
        }
        while (true);

        // A target reported as dropped must not receive the message later
        // after its process resumes and the admitted pipe drains.
        await Task.Delay(TimeSpan.FromSeconds(2));
        var settledA = (await playA.Get("/evidence").Async<string[]>()).Body;
        var settledB = (await playB.Get("/evidence").Async<string[]>()).Body;
        deliveredToA = settledA.Any(line => line.Contains(playAEvidence, StringComparison.Ordinal));
        deliveredToB = settledB.Any(line => line.Contains(playBEvidence, StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            deliveredToA ^ deliveredToB,
            $"SM-C6 {operation} multicast delivery did not match one admitted and one dropped target.");
    }

    static string Evidence(string spotRid, string marker, int sequence)
        => $"|spot={spotRid}|marker={marker}|sequence={sequence}";
}
