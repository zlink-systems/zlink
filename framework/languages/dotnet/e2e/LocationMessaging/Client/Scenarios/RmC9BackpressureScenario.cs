// Verifies RM-C9 Backpressure behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C9 verifies the public inbound application HWM status and recovery path.
internal static class RmC9BackpressureScenario
{
    private const ulong ApplicationHwmBytes = 1UL * 1024 * 1024;
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

        var paused = await WaitForInboundStatusAsync(
            providerA,
            status => status.ApplicationHwmBytes == ApplicationHwmBytes
                      && status.PendingPayloadBytes >= ApplicationHwmBytes
                      && status.ApplicationReceivePaused,
            "RM-C9 provider did not expose pending payload at the application HWM.");
        ZlinkStreamAssert.Ensure(paused.PendingPayloadBytes >= ApplicationHwmBytes,
            "RM-C9 provider status reported less pending payload than the configured HWM.");

        await providerA.Post("/profile/backpressure/release").Async<object>();

        var resumed = await WaitForInboundStatusAsync(
            providerA,
            status => status.PendingPayloadBytes == 0
                      && !status.ApplicationReceivePaused,
            "RM-C9 provider did not resume application receives after handler completion.");
        ZlinkStreamAssert.Ensure(resumed.PendingPayloadBytes == 0,
            "RM-C9 provider retained pending payload after the blocker was released.");

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

    private static async Task<RuntimeInboundStatusRes> WaitForInboundStatusAsync(
        ZLinkHttpClient providerA,
        Func<RuntimeInboundStatusRes, bool> predicate,
        string failureMessage)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        RuntimeInboundStatusRes latest = default!;
        while (DateTimeOffset.UtcNow < deadline)
        {
            latest = (await providerA.Get("/runtime/status")
                .Async<RuntimeInboundStatusRes>()).Body;
            if (predicate(latest)) return latest;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new InvalidOperationException(
            $"{failureMessage} Last status: "
            + $"hwm={latest.ApplicationHwmBytes},pending={latest.PendingPayloadBytes},"
            + $"queued={latest.QueuedPayloadBytes},active={latest.ActivePayloadBytes},"
            + $"paused={latest.ApplicationReceivePaused}.");
    }
}
