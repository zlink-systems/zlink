// Verifies TD-D1 Cross Actor Yield Interleave behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdD1CrossActorYieldInterleaveScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        await context.EnsureActorInSpotAsync(
            actors.ActorA,
            actors.SpotRid,
            "TD-D1-prepare-a");
        await context.EnsureActorInSpotAsync(
            actors.ActorB,
            actors.SpotRid,
            "TD-D1-prepare-b");
        var requestId = ExecutionTurnScenarioContext.NewId("TD-D1");
        var pending = context.ActorRequest(actors.ActorA, new ActorAwaitReq(requestId, 300, "yield"))
            .Async<ActorAwaitRes>();
        await context.EvidenceAsync(requestId, "actor-await-released");
        var fast = context.ActorRequest(actors.ActorB, new ActorFastReq(requestId, "actor-fast"))
            .Async<ActorAwaitRes>();
        await Task.WhenAll(pending.AsTask(), fast.AsTask());
        var evidence = await context.EvidenceAsync(requestId, "actor-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["actor-await-released", "actor-fast-started", "actor-fast-completed", "actor-await-resumed"]);
    }
}
