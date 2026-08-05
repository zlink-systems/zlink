// Verifies TD-E1 Entry To User Spot Join behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdE1EntryToUserSpotJoinScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-E1");
        var reply = await context.ActorRequest(actors.ActorA, new ActorJoinAwaitReq(requestId, actors.SpotRid))
            .Async<ActorAwaitRes>();
        ZlinkStreamAssert.Ensure(
            reply.Marker == "actor-join-await-released",
            "TD-E1 Entry Spot did not defer Actor Join.");
        await context.AssertJoinCompletionAsync(
            requestId,
            "actor-join-await",
            actors.SpotRid,
            "TD-E1");
    }
}
