// Verifies TA-A4 Disconnect And Destroy behavior.
using ToActorMessaging.Client.Support;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaA4DisconnectAndDestroyScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-a4";
        await context.EnsureActorAAsync(actorId);
        await context.CaptureAsync(actorId);
        await using (var bound = await context.ConnectAndBindAsync(context.Options.SessionAStreamEndpoint, actorId))
        {
            await context.AssertBoundPushAsync(bound, null, "TA-A4", actorId, "BeforeDisconnect");
            await bound.Close.Async();
        }
        var afterDisconnect = await context.WaitForSessionUnboundAsync(actorId);
        ZlinkStreamAssert.Ensure(afterDisconnect.All(snapshot => snapshot.SessionRid is null),
            "TA-A4 disconnected actor still had a live bound-session snapshot.");
        await context.AssertBoundPushFailureAsync(actorB: false, "TA-A4", actorId, "AfterDisconnect");
        await context.AssertCallAsync(
            "TA-A4-disconnected-request", actorId, "a4-request", "reply:a4-request", send: false);
        await context.AssertCallAsync("TA-A4-disconnected-send", actorId, "a4-send", "sent", send: true);
        await context.DestroyActorAAsync(actorId, "TA-A4");
        await context.AssertCachedFailureAsync(
            "TA-A4-destroyed-request", actorId, "NotFound");

        var evidence = await context.GetAllActorEvidenceAsync();
        foreach (var (scenario, kind) in new[]
                 {
                     ("TA-A4-disconnected-send", "send"),
                     ("TA-A4-disconnected-request", "request"),
                     ("TA-A4", "destroy")
                 })
            ZlinkStreamAssert.Ensure(
                evidence.Any(item => item.Scenario == scenario && item.Kind == kind),
                $"{scenario} {kind} evidence missing.");
    }
}
