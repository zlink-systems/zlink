// Verifies RC-A4 Di Lifecycle behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-A4 verifies DI lifetimes for dispatch.
internal static class RcA4DiLifecycleScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient evidenceServer)
    {
        var replies = (await requester.Post("/registration/di-filter-order").Async<EchoRes[]>()).Body;
        var first = replies[0];
        var second = replies[1];
        ZlinkStreamAssert.Ensure(
            first.Value == "echo:rc-a4-1" && second.Value == "echo:rc-a4-2",
            "RC-A4 DI reply mismatch.");

        var evidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["di|value=rc-a4-1", "di|value=rc-a4-2"]))
            .Async<string[]>()).Body;
        var di = evidence.Where(line => line.Contains("di|", StringComparison.Ordinal)).ToArray();
        var singletonIds = di.Select(line => EvidenceText.ExtractValue(line, "singleton"))
            .Distinct(StringComparer.Ordinal)
            .Count();
        var scopedIds = di.Select(line => EvidenceText.ExtractValue(line, "scoped"))
            .Distinct(StringComparer.Ordinal)
            .Count();
        ZlinkStreamAssert.Ensure(
            di.Length >= 2 && singletonIds == 1 && scopedIds >= 2,
            "RC-A4 expected stable singleton and per-dispatch scoped dependencies.");

        Console.WriteLine("scenario RC-A4 passed");
    }
}
