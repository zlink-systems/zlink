// Verifies RL-D2 Observer Fault behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-D2 verifies dispatch observer fault isolation and continued messaging.
internal static class RlD2ObserverFaultScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerB.Post("/admin/fault/observer-throws").AsyncRaw();
        var missing = await consumer.Post("/profile/request/missing")
            .Body(new ProfileReq("fast", "rl-d2-error"))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(missing.Status >= 500, "RL-D2 missing handler request should fail.");

        var followUp = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-d2-after"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:fast",
            "RL-D2 messaging did not continue after observer failure.");
        await providerB.Post("/admin/fault/none").AsyncRaw();

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d2-after"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d2-after"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("marker=rl-d2-after", StringComparison.Ordinal)),
                "RL-D2 did not record expected evidence 'marker=rl-d2-after'.");
        }

        Console.WriteLine("scenario RL-D2 passed");
    }
}