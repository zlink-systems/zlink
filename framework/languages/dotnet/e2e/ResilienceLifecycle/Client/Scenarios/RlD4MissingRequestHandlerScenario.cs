// Verifies RL-D4 Missing Request Handler behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-D4 verifies public failure and server evidence for a missing request handler.
internal static class RlD4MissingRequestHandlerScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var failed = await consumer.Post("/profile/request/missing")
            .Body(new ProfileReq("fast", "rl-d4-missing"))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(failed.Status >= 500, "RL-D4 expected public failure for missing request handler.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var request = new EvidenceWaitReq(["dispatch-error|", "packet=MissingProfileReq"], []);
            var waitA = providerA.Post("/evidence/wait").Body(request).Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(request).Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("dispatch-error|", StringComparison.Ordinal)
                                     && line.Contains("packet=MissingProfileReq", StringComparison.Ordinal)),
                "RL-D4 dispatch-error marker missing.");
        }

        Console.WriteLine("scenario RL-D4 passed");
    }
}