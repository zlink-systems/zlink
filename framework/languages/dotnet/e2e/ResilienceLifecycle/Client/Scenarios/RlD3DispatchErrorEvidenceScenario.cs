// Verifies RL-D3 Dispatch Error Evidence behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-D3 verifies dispatch-error evidence for missing send handling.
internal static class RlD3DispatchErrorEvidenceScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var failed = await consumer.Post("/profile/request/missing")
            .Body(new ProfileReq("fast", "rl-d3-missing"))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(failed.Status >= 500, "RL-D3 expected missing request handler failure.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var request = new EvidenceWaitReq(["dispatch-error|"], [["packet=MissingProfileReq"]]);
            var waitA = providerA.Post("/evidence/wait").Body(request).Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(request).Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("packet=MissingProfileReq", StringComparison.Ordinal)),
                "RL-D3 dispatch-error evidence did not include MissingProfileReq.");
        }

        Console.WriteLine("scenario RL-D3 passed");
    }
}