// Verifies RL-B6 Gray Fault behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-B6 verifies mixed gray failures, healthy-provider success, and recovery.
internal static class RlB6GrayFaultScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerB.Post("/admin/fault/gray").AsyncRaw();

        var failures = 0;
        var successes = 0;
        for (var i = 0; i < 30; i++)
            try
            {
                var reply = (await consumer.Post("/profile/request")
                    .Body(new ProfileReq(i % 3 == 0 ? "gray" : "fast", $"rl-b6-{i}"))
                    .Async<ProfileRes>()).Body;
                if (reply.ProviderRid == "api-a") successes++;
            }
            catch
            {
                failures++;
            }

        ZlinkStreamAssert.Ensure(successes > 0 && failures > 0, "RL-B6 expected both healthy successes and gray failures.");
        await providerB.Post("/admin/fault/none").AsyncRaw();
        var followUp = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-b6-after"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:fast", "RL-B6 follow-up request failed after clearing fault.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-b6-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-b6-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("marker=rl-b6-", StringComparison.Ordinal)),
                "RL-B6 did not record expected evidence 'marker=rl-b6-'.");
        }
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-b6-after"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-b6-after"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("marker=rl-b6-after", StringComparison.Ordinal)),
                "RL-B6 did not record expected evidence 'marker=rl-b6-after'.");
        }

        Console.WriteLine("scenario RL-B6 passed");
    }
}