// Verifies RM-C5 Missing Packet behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;
using Zlink.Framework.Contracts.Errors;

namespace LocationMessaging.Client.Scenarios;

// RM-C5 verifies that missing packet registrations are reported as dispatch
// errors and that normal traffic still works afterward.
internal static class RmC5MissingPacketScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient storeConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var missingRequest = (await storeConsumer.Post("/profile/missing-request")
            .Body(new ProfileReq("missing-request"))
            .Async<ExpectedFailureRes>()).Body;
        ZlinkStreamAssert.Ensure(
            missingRequest.ErrorKind == nameof(ZLinkFrameworkErrorKind.NotFound),
            "RM-C5 missing request should report NotFound.");

        await storeConsumer.Post("/profile/missing-command")
            .Body(new ProfileMsg("missing-send"))
            .Async<object>();

        var evidence = (await WaitForDispatchErrorEvidenceAsync(
                providerA,
                providerB,
                "MissingProfileReq"))
            .Concat(await WaitForDispatchErrorEvidenceAsync(
                providerA,
                providerB,
                "MissingProfileMsg"))
            .ToArray();
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("dispatch-error", StringComparison.Ordinal)
                                 && line.Contains("kind=Request", StringComparison.Ordinal)
                                 && line.Contains("reason=HandlerMissing", StringComparison.Ordinal)
                                 && line.Contains("action=ReplyError", StringComparison.Ordinal)
                                 && line.Contains("packet=MissingProfileReq", StringComparison.Ordinal)),
            "RM-C5 missing request evidence should report HandlerMissing/ReplyError.");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("dispatch-error", StringComparison.Ordinal)
                                 && line.Contains("kind=Send", StringComparison.Ordinal)
                                 && line.Contains("reason=HandlerMissing", StringComparison.Ordinal)
                                 && line.Contains("action=Drop", StringComparison.Ordinal)
                                 && line.Contains("packet=MissingProfileMsg", StringComparison.Ordinal)),
            "RM-C5 missing send evidence should report HandlerMissing/Drop.");

        var reply = (await storeConsumer.Post("/profile/request")
            .Body(new ProfileReq("rm-c5-after"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.Value == "profile:rm-c5-after", "RM-C5 normal request after negative path failed.");
    }

    private static async Task<string[]> WaitForDispatchErrorEvidenceAsync(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        string packetName)
    {
        return await ProviderEvidence.WaitFromEitherAsync(providerA, providerB, packetName);
    }
}
