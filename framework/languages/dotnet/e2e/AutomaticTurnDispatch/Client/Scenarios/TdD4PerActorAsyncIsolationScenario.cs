// Verifies that PerActor Async blocks only the current Actor lane.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal static class TdD4PerActorAsyncIsolationScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync(AutomaticTurnDispatchNames.PerActorSpotType);
        await context.EnsureActorInSpotAsync(
            actors.ActorA,
            actors.SpotRid,
            "TD-D4-prepare-a",
            AutomaticTurnDispatchNames.PerActorSpotType);
        await context.EnsureActorInSpotAsync(
            actors.ActorB,
            actors.SpotRid,
            "TD-D4-prepare-b",
            AutomaticTurnDispatchNames.PerActorSpotType);

        var requestId = ExecutionTurnScenarioContext.NewId("TD-D4");
        var held = context.ActorRequest(
                actors.ActorA,
                new PerActorAwaitReq(requestId, 300, "async"))
            .Async<ActorAwaitRes>();
        await context.EvidenceAsync(requestId, "per-actor-await-held");

        var actorB = context.ActorRequest(
                actors.ActorB,
                new PerActorFastReq(requestId, "actor-b-fast"))
            .Async<ActorAwaitRes>();
        var actorA = context.ActorRequest(
                actors.ActorA,
                new PerActorFastReq(requestId, "actor-a-follow-up"))
            .Async<ActorAwaitRes>();
        var bReply = await actorB;
        ZlinkStreamAssert.Ensure(
            bReply.Marker == "actor-b-fast",
            "TD-D4 Actor B did not complete while Actor A was awaiting.");

        await held;
        var aReply = await actorA;
        ZlinkStreamAssert.Ensure(
            aReply.Marker == "actor-a-follow-up",
            "TD-D4 Actor A follow-up did not remain FIFO after the first handler.");

        var evidence = await context.EvidenceAsync(requestId, "per-actor-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(
            evidence,
            requestId,
            ["per-actor-await-held", "per-actor-fast-completed"]);
    }
}
