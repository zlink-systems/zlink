// Verifies TD-E2 User To User Spot Join behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdE2UserToUserSpotJoinScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        await context.EnsureActorInSpotAsync(actors.ActorA, actors.SpotRid, "TD-E2-prepare");
        var target = $"td-e2-target-{Guid.NewGuid():N}";
        await context.EnsureSpotAsync(target, "play-a");
        var requestId = ExecutionTurnScenarioContext.NewId("TD-E2");
        var reply = await context.ActorRequest(actors.ActorA,
                new ActorJoinAwaitReq(requestId, target))
            .Async<ActorAwaitRes>();
        ZlinkStreamAssert.Ensure(
            reply.Marker == "actor-join-deferred",
            "TD-E2 User Spot did not defer Actor Join.");
        await context.AssertJoinCompletionAsync(requestId, "actor-join", target, "TD-E2");
    }
}
