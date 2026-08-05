// Verifies RM-C2 Targeted Route behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C2 verifies route-targeted request delivery to a specific provider rid
// and bounded failure for a missing rid.
internal static class RmC2TargetedRouteScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var marker = $"rm-c2-{Guid.NewGuid():N}";

        var reply = (await providerA.Post("/profile/route/request")
            .Body(new ScenarioRoutePing(marker))
            .Async<ScenarioRoutePong>()).Body;
        ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-b", "RM-C2 targeted route request should reach api-b.");
        ZlinkStreamAssert.Ensure(reply.Value == $"route:{marker}", "RM-C2 route reply value mismatch.");

        var afterB = (await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(marker))
            .Async<string[]>()).Body;
        var afterA = (await providerA.Get("/evidence").Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            afterB.Count(line => line.Contains("route-request|rid=api-b", StringComparison.Ordinal)
                                 && line.Contains(marker, StringComparison.Ordinal)) == 1
            && afterA.Count(line => line.Contains("route-request|rid=api-a", StringComparison.Ordinal)
                                    && line.Contains(marker, StringComparison.Ordinal)) == 0,
            "RM-C2 targeted route evidence did not match api-b only.");

        var missing = (await providerA.Post("/profile/route/missing")
            .Body(new ScenarioRoutePing("missing"))
            .Async<ExpectedFailureRes>()).Body;
        ZlinkStreamAssert.Ensure(
            missing.ErrorKind == nameof(ZLinkFrameworkErrorKind.NotFound),
            "RM-C2 missing rid request should report NotFound.");
    }
}
