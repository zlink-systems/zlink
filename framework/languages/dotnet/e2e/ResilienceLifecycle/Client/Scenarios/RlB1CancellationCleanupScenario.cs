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
        var firstMarker = $"rl-b1-first-{Guid.NewGuid():N}";
        await providerA.Post($"/admin/profile/hold/{firstMarker}").AsyncRaw();
        await providerB.Post($"/admin/profile/hold/{firstMarker}").AsyncRaw();

        using var firstCancellation = new CancellationTokenSource();
        using var evidenceTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var firstTask = consumer.Post("/profile/request")
            .Body(new ProfileReq("first", firstMarker))
            .Async<ProfileRes>(firstCancellation.Token)
            .AsTask();
        var waitA = providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-a|marker={firstMarker}"], []))
            .Async<string[]>(evidenceTimeout.Token)
            .AsTask();
        var waitB = providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={firstMarker}"], []))
            .Async<string[]>(evidenceTimeout.Token)
            .AsTask();
        var completedEvidence = await Task.WhenAny(waitA, waitB);
        await completedEvidence;

        firstCancellation.Cancel();
        await AssertCanceledAsync(firstTask);

        var followUpMarker = $"rl-b1-follow-up-{Guid.NewGuid():N}";
        var followUp = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", followUpMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:fast", "RL-B1 follow-up request failed after timeout.");

        await providerA.Post($"/admin/profile/release/{firstMarker}").AsyncRaw();
        await providerB.Post($"/admin/profile/release/{firstMarker}").AsyncRaw();
        evidenceTimeout.Cancel();

        using (var lateReplyTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)))
        {
            var lateWaitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|", $"marker={firstMarker}"], []))
                .Async<string[]>(lateReplyTimeout.Token).AsTask();
            var lateWaitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|", $"marker={firstMarker}"], []))
                .Async<string[]>(lateReplyTimeout.Token).AsTask();
            var completed = await Task.WhenAny(lateWaitA, lateWaitB);
            var evidence = (await completed).Body;
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains($"marker={firstMarker}", StringComparison.Ordinal)),
                "RL-B1 first request completion evidence missing after gate release.");
        }

        var later = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", $"rl-b1-later-{Guid.NewGuid():N}"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(later.Value == "profile:fast", "RL-B1 later request failed after slow completion.");

        Console.WriteLine("scenario RL-B1 passed");
    }

    private static async Task AssertCanceledAsync(Task firstTask)
    {
        try
        {
            await firstTask;
        }
        catch (OperationCanceledException)
        {
            return;
        }

        throw new InvalidOperationException("RL-B1 first request was not canceled.");
    }
}
