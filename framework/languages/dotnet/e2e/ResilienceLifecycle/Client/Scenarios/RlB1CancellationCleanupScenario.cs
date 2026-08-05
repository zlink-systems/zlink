// Verifies RL-B1 Cancellation Cleanup behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-B1 verifies timeout/cancellation cleanup and a successful follow-up request.
internal static class RlB1CancellationCleanupScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var slowMarker = $"rl-b1-slow-{Guid.NewGuid():N}";
        var timeout = await consumer.Post("/profile/request/timeout/100")
            .Body(new ProfileReq("slow", slowMarker))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(timeout.Status == 408, "RL-B1 expected the slow request to time out.");

        var followUpMarker = $"rl-b1-follow-up-{Guid.NewGuid():N}";
        var followUp = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", followUpMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:fast", "RL-B1 follow-up request failed after timeout.");

        using (var evidenceTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)))
        {
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|", $"marker={slowMarker}"], []))
                .Async<string[]>(evidenceTimeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|", $"marker={slowMarker}"], []))
                .Async<string[]>(evidenceTimeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            evidenceTimeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains($"marker={slowMarker}", StringComparison.Ordinal)),
                "RL-B1 slow request completion evidence missing.");
        }

        var later = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", $"rl-b1-later-{Guid.NewGuid():N}"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(later.Value == "profile:fast", "RL-B1 later request failed after slow completion.");

        Console.WriteLine("scenario RL-B1 passed");
    }
}