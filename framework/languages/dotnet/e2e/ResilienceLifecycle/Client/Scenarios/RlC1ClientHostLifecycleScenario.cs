// Verifies RL-C1 Client Host Lifecycle behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-C1 verifies repeated client lifecycle traffic and a follow-up request.
internal static class RlC1ClientHostLifecycleScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        foreach (var index in Enumerable.Range(0, 12))
        {
            var reply = await EphemeralRouteClient.RequestAsync(
                options,
                new ProfileReq("fast", $"rl-c1-{index}"));
            ZlinkStreamAssert.Ensure(
                reply.Value == "profile:fast",
                "RL-C1 request failed before cleanup.");
        }

        var followUp = await EphemeralRouteClient.RequestAsync(
            options,
            new ProfileReq("fast", "rl-c1-after-cleanup"));
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:fast", "RL-C1 follow-up failed after client cleanup.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-c1-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-c1-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("marker=rl-c1-", StringComparison.Ordinal)),
                "RL-C1 did not record expected evidence 'marker=rl-c1-'.");
        }
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c1-after-cleanup"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c1-after-cleanup"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c1-after-cleanup", StringComparison.Ordinal)),
                "RL-C1 did not record expected evidence 'marker=rl-c1-after-cleanup'.");
        }

        Console.WriteLine("scenario RL-C1 passed");
    }
}
