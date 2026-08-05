// Verifies TA-A1 Bound Actor Messaging behavior.
using ToActorMessaging.Client.Support;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaA1BoundActorMessagingScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-a1";
        await context.EnsureActorAAsync(actorId);
        await using var bound = await context.ConnectAndBindAsync(context.Options.SessionAStreamEndpoint, actorId);
        await using var unbound = await context.ConnectAsync(context.Options.SessionBStreamEndpoint);
        var bindingBefore = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(bindingBefore.Count(snapshot => snapshot.SessionRid is not null) == 1,
            "TA-A1 expected exactly one bound session before no-bind messaging.");
        await context.AssertBoundPushAsync(bound, unbound, "TA-A1", actorId, "BeforeNotify");
        await context.AssertCallAsync("TA-A1-send", actorId, "a1-send", "sent", send: true);
        await context.AssertCallAsync("TA-A1-request", actorId, "a1-request", "reply:a1-request", send: false);
        await context.AssertBoundPushAsync(bound, unbound, "TA-A1", actorId, "AfterNotify");
        var bindingAfter = await context.GetBoundSessionSnapshotsAsync(actorId);
        ZlinkStreamAssert.Ensure(bindingBefore.SequenceEqual(bindingAfter),
            "TA-A1 no-bind messaging changed the bound-session snapshot.");

        var evidence = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(evidence.Any(item => item is
            { Scenario: "TA-A1-send", Kind: "send" }), "TA-A1 send evidence missing.");
        ZlinkStreamAssert.Ensure(evidence.Any(item => item is
            { Scenario: "TA-A1-request", Kind: "request" }), "TA-A1 request evidence missing.");
        ZlinkStreamAssert.Ensure(evidence.Any(item => item is
            { Scenario: "TA-A1", Kind: "bound-push", Value: "BeforeNotify" }),
            "TA-A1 BeforeNotify bound push evidence missing.");
        ZlinkStreamAssert.Ensure(evidence.Any(item => item is
            { Scenario: "TA-A1", Kind: "bound-push", Value: "AfterNotify" }),
            "TA-A1 AfterNotify bound push evidence missing.");
    }
}
