// Verifies TA-B2 Stale Actor Reference behavior.
using ToActorMessaging.Client.Support;
using ToActorMessaging.Shared;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaB2StaleActorReferenceScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-b2";
        await context.EnsureActorAAsync(actorId);
        var staleRef = await context.CaptureAsync(actorId);
        await context.DestroyActorAAsync(actorId, "TA-B2");
        await context.WaitForRouteAbsentAsync(actorId);
        await context.EnsureActorAAsync(actorId);

        await context.AssertCachedFailureAsync("TA-B2-stale-request", actorId, "NotFound");
        var afterStale = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(afterStale.All(item => item.Scenario != "TA-B2-stale-request"),
            "TA-B2 stale generation unexpectedly reached an actor handler.");
        var freshRef = await context.CaptureAsync(actorId);
        ZlinkStreamAssert.Ensure(
            freshRef.NodeRid == staleRef.NodeRid && freshRef.ObjectGeneration > staleRef.ObjectGeneration,
            $"TA-B2 expected a newer generation on the same owner. stale={staleRef}, fresh={freshRef}");
        await context.AssertCallAsync(
            "TA-B2-fresh-request", actorId, "b2-fresh", "reply:b2-fresh", send: false);
        var afterFresh = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(afterFresh.Any(item => item is
        {
            Scenario: "TA-B2-fresh-request",
            Kind: "request",
            NodeRid: "actor-a",
            PacketName: nameof(ActorAsk)
        } && item.ActorId == actorId && item.Generation == freshRef.ObjectGeneration),
            "TA-B2 fresh request generation/owner evidence missing.");
    }
}
