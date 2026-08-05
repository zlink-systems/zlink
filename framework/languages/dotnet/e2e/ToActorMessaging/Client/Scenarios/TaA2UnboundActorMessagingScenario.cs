// Verifies TA-A2 Unbound Actor Messaging behavior.
using ToActorMessaging.Client.Support;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaA2UnboundActorMessagingScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-a2";
        await context.EnsureActorBAsync(actorId);
        var bindingBefore = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(bindingBefore.All(snapshot => snapshot.SessionRid is null),
            "TA-A2 actor unexpectedly had a bound session before no-bind messaging.");
        await context.AssertCallAsync("TA-A2-unbound-send", actorId, "a2-send", "sent", send: true);
        await context.AssertCallAsync("TA-A2-unbound-request", actorId, "a2-request", "reply:a2-request", send: false);
        await context.AssertBoundPushFailureAsync(actorB: true, "TA-A2", actorId, "must-remain-unbound");
        var bindingAfter = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(bindingBefore.SequenceEqual(bindingAfter),
            "TA-A2 no-bind messaging created or changed a bound session.");

        var evidence = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(evidence.Any(item => item is
            { Scenario: "TA-A2-unbound-send", Kind: "send" }), "TA-A2 send evidence missing.");
    }
}
