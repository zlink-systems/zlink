// Verifies TD-D2 Same Actor No Reentry behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdD2SameActorNoReentryScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        await context.EnsureActorInSpotAsync(
            actors.ActorA,
            actors.SpotRid,
            "TD-D2-prepare");
        var requestId = ExecutionTurnScenarioContext.NewId("TD-D2");
        var pending = context.ActorRequest(actors.ActorA, new ActorAwaitReq(requestId, 300, "yield"))
            .Async<ActorAwaitRes>();
        await context.EvidenceAsync(requestId, "actor-await-released");
        var fast = context.ActorRequest(actors.ActorA, new ActorFastReq(requestId, "actor-fast"))
            .Async<ActorAwaitRes>();
        await Task.WhenAll(pending.AsTask(), fast.AsTask());
        var evidence = await context.EvidenceAsync(requestId, "actor-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["actor-await-released", "actor-await-resumed", "actor-await-completed", "actor-fast-started"]);
    }
}
