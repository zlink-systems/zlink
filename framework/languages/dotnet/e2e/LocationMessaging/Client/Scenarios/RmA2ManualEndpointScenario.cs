// Verifies RM-A2 Manual Endpoint behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-A2 verifies that a consumer can request a profile provider through an
// explicitly configured peer endpoint without registering a location store.
internal static class RmA2ManualEndpointScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient singleConsumer,
        ZLinkHttpClient providerA)
    {
        var reply = (await singleConsumer.Post("/profile/request").Body(new ProfileReq("rm-a2"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.Value == "profile:rm-a2", "RM-A2 reply value mismatch.");
        ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-a", "RM-A2 manual endpoint should reach api-a.");

        var evidence = (await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq("value=rm-a2"))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("value=rm-a2", StringComparison.Ordinal)),
            "RM-A2 api-a evidence missing.");
    }
}
