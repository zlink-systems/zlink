// Verifies RM-C4 Timeout Isolation behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C4 verifies that a timed-out request does not poison later requests on
// the same location-store auto-connected channel.
internal static class RmC4TimeoutIsolationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient storeConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var timeout = (await storeConsumer.Post("/profile/slow-request")
            .Body(new ProfileReq("slow"))
            .Async<ExpectedFailureRes>()).Body;
        ZlinkStreamAssert.Ensure(
            timeout.ErrorKind == nameof(ZLinkFrameworkErrorKind.DeadlineExceeded),
            "RM-C4 expected DeadlineExceeded.");

        var immediate = (await storeConsumer.Post("/profile/request")
            .Body(new ProfileReq("rm-c4-after-timeout"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(immediate.Value == "profile:rm-c4-after-timeout", "RM-C4 follow-up reply mismatch.");

        // The timed-out request may still be in flight (dealer connect or
        // handler completion after the client-side timeout); the completion
        // evidence gets a wider window than the default poll.
        var slowCompletion = await ProviderEvidence.WaitFromEitherAsync(
            providerA, providerB, "value=slow", timeoutMilliseconds: 30000);
        ZlinkStreamAssert.Ensure(
            slowCompletion.Any(line => line.Contains("profile-request", StringComparison.Ordinal)
                                       && line.Contains("value=slow", StringComparison.Ordinal)),
            "RM-C4 timed-out handler did not complete on the server.");

        var later = (await storeConsumer.Post("/profile/request")
            .Body(new ProfileReq("rm-c4-later"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(later.Value == "profile:rm-c4-later", "RM-C4 later reply mismatch.");

        var afterTimeoutEvidence = await ProviderEvidence.WaitFromEitherAsync(
            providerA,
            providerB,
            "rm-c4-after-timeout");
        var laterEvidence = await ProviderEvidence.WaitFromEitherAsync(providerA, providerB, "rm-c4-later");
        var evidence = afterTimeoutEvidence.Concat(laterEvidence).ToArray();
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("rm-c4-after-timeout", StringComparison.Ordinal))
            && evidence.Any(line => line.Contains("rm-c4-later", StringComparison.Ordinal)),
            "RM-C4 follow-up evidence missing.");
    }

}
