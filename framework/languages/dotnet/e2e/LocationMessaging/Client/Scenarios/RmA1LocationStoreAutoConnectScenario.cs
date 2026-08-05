// Verifies RM-A1 Location Store Auto Connect behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-A1 verifies that an endpoint-less channel client auto-connects from the
// shared location store and that both providers' peer location rows are alive:
// Both endpoints use the public operational Location query; the second path
// verifies that callers do not need the removed public resolver abstraction.
internal static class RmA1LocationStoreAutoConnectScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var reply = (await providerA.Post("/profile/request")
            .Body(new ProfileReq("rm-a1"))
            .Async<ProfileRes>()).Body;

        ZlinkStreamAssert.Ensure(reply.Value == "profile:rm-a1", "RM-A1 reply value mismatch.");
        ZlinkStreamAssert.Ensure(reply.ProviderRid is "api-a" or "api-b", "RM-A1 provider rid was not api-a/api-b.");

        var rawPeers = (await providerA.Get("/locations/peers?mesh=profile").Async<PeerLocationRow[]>()).Body;
        var liveProviderRows = rawPeers.Count(row =>
            row.Role == "Router" && IsProviderRid(row.NodeRid));
        ZlinkStreamAssert.Ensure(
            liveProviderRows >= 2,
            "RM-A1 expected live peer location rows for both profile providers in the runtime query.");

        var memberPeers = (await providerA.Get("/locations/member-peers?mesh=profile").Async<PeerLocationRow[]>()).Body;
        ZlinkStreamAssert.Ensure(
            memberPeers.Count(row => row.Role == "Router" && IsProviderRid(row.NodeRid)) >= 2,
            "RM-A1 expected both providers on the public operational query surface.");

        var status = (await providerA.Get("/locations/status").Async<LocationStatusRes>()).Body;
        ZlinkStreamAssert.Ensure(status.StoreHealthy, "RM-A1 expected a healthy location store.");

        var providerEvidence = (await providerA.Get("/evidence").Async<string[]>()).Body
            .Concat((await providerB.Get("/evidence").Async<string[]>()).Body)
            .ToArray();
        ZlinkStreamAssert.Ensure(
            providerEvidence.Any(line => line.Contains("value=rm-a1", StringComparison.Ordinal)),
            "RM-A1 provider evidence missing.");
    }

    private static bool IsProviderRid(string? value) =>
        value is not null
        && (value.StartsWith("api-a-", StringComparison.Ordinal)
            || value.StartsWith("api-b-", StringComparison.Ordinal));
}
