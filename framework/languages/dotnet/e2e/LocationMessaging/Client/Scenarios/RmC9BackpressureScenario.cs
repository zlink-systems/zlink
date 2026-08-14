// Verifies RM-C9 Backpressure behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C9 verifies retained inbound ownership is released when the handler completes.
internal static class RmC9BackpressureScenario
{
    private const int BlockerPayloadBytes = 2 * 1024 * 1024;

    public static async Task RunAsync(ZLinkHttpClient backpressureConsumer, ZLinkHttpClient providerA)
    {
        await providerA.Post("/profile/backpressure/reset").Async<object>();
        var marker = $"rm-c9-{Guid.NewGuid():N}";
        var payload = new string('x', BlockerPayloadBytes);
        var commandId = $"rm-c9-block-{marker}";
        var outcome = await SendBackpressureCommandAsync(
            backpressureConsumer,
            new BackpressureMsg(commandId, payload));
        ZlinkStreamAssert.Ensure(outcome == "Submitted",
            "RM-C9 blocker command was not submitted through the public consumer endpoint.");

        await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq($"profile-command-start|rid=api-a|command={commandId}", 30000))
            .Async<string[]>();

        await providerA.Post("/profile/backpressure/release").Async<object>();

        var followUp = (await backpressureConsumer.Post("/profile/request")
            .Body(new ProfileReq("rm-c9-after"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:rm-c9-after",
            "RM-C9 follow-up request failed after backlog cleared.");

        var recoveryEvidence = (await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq("rm-c9-after", 20000))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            recoveryEvidence.Any(line => line.Contains("rm-c9-after", StringComparison.Ordinal)),
            "RM-C9 recovery evidence missing.");
    }

    private static async Task<string> SendBackpressureCommandAsync(
        ZLinkHttpClient backpressureConsumer,
        BackpressureMsg command)
    {
        return (await backpressureConsumer.Post("/profile/backpressure/send")
            .Body(command)
            .Async<string>()).Body;
    }

}
