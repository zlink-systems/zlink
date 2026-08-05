// Verifies RL-D1 High Fanout behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-D1 verifies a high-fanout request burst over the channel.
internal static class RlD1HighFanoutScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var tasks = Enumerable.Range(0, 120).Select(async i =>
        {
            return (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-d1-{i}"))
                .Async<ProfileRes>()).Body;
        });
        var replies = await Task.WhenAll(tasks);
        ZlinkStreamAssert.Ensure(
            replies.Length == 120 && replies.All(reply => reply.Value == "profile:fast"),
            "RL-D1 high request fanout did not complete.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d1-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d1-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("marker=rl-d1-", StringComparison.Ordinal)),
                "RL-D1 did not record expected evidence 'marker=rl-d1-'.");
        }

        Console.WriteLine("scenario RL-D1 passed");
    }
}