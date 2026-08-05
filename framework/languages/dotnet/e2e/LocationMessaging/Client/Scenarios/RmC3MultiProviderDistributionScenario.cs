// Verifies RM-C3 Multi Provider Distribution behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C3 verifies that direct multi-endpoint channel configuration distributes
// profile requests across both configured providers.
internal static class RmC3MultiProviderDistributionScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient directConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var beforeA = (await providerA.Get("/evidence").Async<string[]>()).Body;
        var beforeB = (await providerB.Get("/evidence").Async<string[]>()).Body;
        var marker = $"rm-c3-{Guid.NewGuid():N}";
        var requests = Enumerable.Range(0, 60)
            .Select(index => new ProfileReq($"{marker}-{index}"))
            .ToArray();

        var replies = (await directConsumer.Post("/profile/batch-request")
            .Body(requests)
            .Async<ProfileRes[]>()).Body;
        ZlinkStreamAssert.Ensure(replies.Length == requests.Length, "RM-C3 reply count mismatch.");
        for (var i = 0; i < requests.Length; i++)
        {
            ZlinkStreamAssert.Ensure(replies[i].Value == $"profile:{marker}-{i}", "RM-C3 reply value mismatch.");
            ZlinkStreamAssert.Ensure(replies[i].ProviderRid is "api-a" or "api-b", "RM-C3 reply provider mismatch.");
        }

        var afterA = (await providerA.Get("/evidence").Async<string[]>()).Body;
        var afterB = (await providerB.Get("/evidence").Async<string[]>()).Body;
        var a = EvidenceDelta.CountMatching(afterA, beforeA, "profile-request|rid=api-a", marker);
        var b = EvidenceDelta.CountMatching(afterB, beforeB, "profile-request|rid=api-b", marker);
        ZlinkStreamAssert.Ensure(
            a > 0 && b > 0 && a + b == requests.Length,
            "RM-C3 expected both providers to handle the direct multi-endpoint request set.");
    }
}
