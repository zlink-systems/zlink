// Verifies TA-B3 Route Reconnect behavior.
using ToActorMessaging.Client.Support;
using ToActorMessaging.Shared;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaB3RouteReconnectScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "ta-b3";
        await context.EnsureActorAAsync(actorId);
        var actor = await context.CaptureAsync(actorId);
        await context.AssertNoRouteCallerFailureAsync("TA-B3-no-route-mesh", actor);
        var disconnectedEvidence = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(
            disconnectedEvidence.All(item => item.Scenario != "TA-B3-no-route-mesh"),
            "TA-B3 disconnected request unexpectedly reached an actor handler.");
        await context.AssertNoRouteCallerRecoveryAsync("TA-B3-recovered-request", actor, "b3-recovered");
        var recoveredEvidence = await context.GetAllActorEvidenceAsync();
        ZlinkStreamAssert.Ensure(recoveredEvidence.Any(item => item is
        {
            Scenario: "TA-B3-recovered-request",
            Kind: "request",
            NodeRid: "actor-a",
            PacketName: nameof(ActorAsk)
        } && item.ActorId == actorId), "TA-B3 recovered request actor-owner evidence missing.");
    }
}
