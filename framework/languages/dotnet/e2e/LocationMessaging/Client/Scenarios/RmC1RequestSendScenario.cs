// Verifies RM-C1 Request Send behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C1 verifies the normal request/reply and one-way send paths for a
// location-store auto-connected profile channel.
internal static class RmC1RequestSendScenario
{
    public static async Task RunAsync(ZLinkHttpClient providerA, ZLinkHttpClient providerB)
    {
        try
        {
            // Provider A remains the caller but its local server membership
            // is excluded. The public calls below still name only "profile".
            await providerA.Post("/profile/weight")
                .Query("weight", "0")
                .AsyncRaw();

            var reply = (await providerA.Post("/profile/request")
                .Body(new ProfileReq("rm-c1-request"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(
                reply.Value == "profile:rm-c1-request"
                && reply.ProviderRid == "api-b",
                "RM-C1 request did not complete at the remote Channel server.");

            var commandId = $"cmd-{Guid.NewGuid():N}";
            await providerA.Post("/profile/command")
                .Body(new ProfileMsg(commandId))
                .Async<object>();

            var remoteEvidence = (await providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq($"command={commandId}"))
                .Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                remoteEvidence.Any(line =>
                    line.Contains("profile-request|", StringComparison.Ordinal)
                    && line.Contains(
                        "rm-c1-request",
                        StringComparison.Ordinal))
                && remoteEvidence.Any(line =>
                    line.Contains(
                        $"profile-command|rid=api-b|command={commandId}",
                        StringComparison.Ordinal)),
                "RM-C1 remote request/send evidence missing.");

            var sourceEvidence =
                (await providerA.Get("/evidence").Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                sourceEvidence.All(line =>
                    !line.Contains(
                        "value=rm-c1-request",
                        StringComparison.Ordinal)
                    && !line.Contains(
                        $"command={commandId}",
                        StringComparison.Ordinal)),
                "RM-C1 source handled a ChannelName call after local exclusion.");
        }
        finally
        {
            await providerA.Post("/profile/weight")
                .Query("weight", "100")
                .AsyncRaw();
        }
    }
}
