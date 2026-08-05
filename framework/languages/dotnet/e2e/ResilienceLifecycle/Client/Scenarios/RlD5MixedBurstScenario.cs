// Verifies RL-D5 Mixed Burst behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-D5 verifies mixed request/send burst traffic.
internal static class RlD5MixedBurstScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var requestTasks = Enumerable.Range(0, 60).Select(async i =>
        {
            return (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-d5-req-{i}"))
                .Async<ProfileRes>()).Body;
        });
        var sendTasks = Enumerable.Range(0, 60).Select(i =>
            consumer.Post("/profile/command")
                .Body(new ProfileMsg($"rl-d5-cmd-{i}"))
                .AsyncRaw().AsTask());
        var replies = await Task.WhenAll(requestTasks);
        await Task.WhenAll(sendTasks);
        ZlinkStreamAssert.Ensure(
            replies.All(reply => reply.Value == "profile:fast"),
            "RL-D5 mixed workload replies were invalid.");

        string[] requestEvidence;
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d5-req-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d5-req-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            requestEvidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                requestEvidence.Any(line => line.Contains("marker=rl-d5-req-", StringComparison.Ordinal)),
                "RL-D5 evidence missing for marker=rl-d5-req-.");
        }

        string[] commandEvidence;
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d5-cmd-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-d5-cmd-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            commandEvidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                commandEvidence.Any(line => line.Contains("marker=rl-d5-cmd-", StringComparison.Ordinal)),
                "RL-D5 evidence missing for marker=rl-d5-cmd-.");
        }

        var evidence = requestEvidence.Concat(commandEvidence).ToArray();
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("marker=rl-d5-req-", StringComparison.Ordinal)),
            "RL-D5 did not record expected evidence 'marker=rl-d5-req-'.");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("marker=rl-d5-cmd-", StringComparison.Ordinal)),
            "RL-D5 did not record expected evidence 'marker=rl-d5-cmd-'.");

        Console.WriteLine("scenario RL-D5 passed");
    }
}