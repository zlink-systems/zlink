// Verifies TA-A3 Late Bind behavior.
using ToActorMessaging.Client.Support;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaA3LateBindScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-a3";
        await context.EnsureActorAAsync(actorId);
        var beforeBind = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(beforeBind.All(snapshot => snapshot.SessionRid is null),
            "TA-A3 actor unexpectedly had a session before the explicit bind.");
        await context.AssertCallAsync("TA-A3-before-bind-send", actorId, "a3-before-send", "sent", send: true);
        await context.AssertCallAsync(
            "TA-A3-before-bind-request", actorId, "a3-before-request", "reply:a3-before-request", send: false);
        await context.AssertBoundPushFailureAsync(
            actorB: false, "TA-A3-before-bind", actorId, "must-remain-unbound");
        await using var bound = await context.ConnectAndBindAsync(context.Options.SessionBStreamEndpoint, actorId);
        var afterBind = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(afterBind.Count(snapshot => snapshot.SessionRid is not null) == 1,
            "TA-A3 explicit bind did not create exactly one bound-session snapshot.");
        await context.AssertCallAsync("TA-A3-after-bind-send", actorId, "a3-after-send", "sent", send: true);
        await context.AssertCallAsync(
            "TA-A3-after-bind-request", actorId, "a3-after-request", "reply:a3-after-request", send: false);
        await context.AssertBoundPushAsync(bound, null, "TA-A3", actorId, "LateBindNotify");
        var afterNoBindCalls = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(afterBind.SequenceEqual(afterNoBindCalls),
            "TA-A3 post-bind no-bind messaging changed the explicit session binding.");

        var evidence = await context.GetAllActorEvidenceAsync();
        foreach (var (scenario, kind) in new[]
                 {
                     ("TA-A3-before-bind-send", "send"),
                     ("TA-A3-before-bind-request", "request"),
                     ("TA-A3-after-bind-send", "send"),
                     ("TA-A3-after-bind-request", "request"),
                     ("TA-A3", "bound-push")
                 })
            ZlinkStreamAssert.Ensure(
                evidence.Any(item => item.Scenario == scenario && item.Kind == kind),
                $"{scenario} {kind} evidence missing.");
    }
}
