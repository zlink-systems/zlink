// Verifies TD-F1 Remote Spot Continuation behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdF1RemoteSpotContinuationScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        await context.SetExclusivePlacementAsync("play-a");
        try
        {
            var owner = $"td-f1-owner-{Guid.NewGuid():N}";
            var ownerNode = await context.EnsureSpotNodeAsync(owner, "play-a");
            ZlinkStreamAssert.Ensure(ownerNode == "play-a", "TD-F1 source Spot was not placed on play-a.");

            await context.SetExclusivePlacementAsync("play-b");
            var target = $"td-f1-target-{Guid.NewGuid():N}";
            var targetNode = await context.EnsureSpotNodeAsync(target, "play-b");
            ZlinkStreamAssert.Ensure(targetNode == "play-b", "TD-F1 target Spot was not placed on play-b.");
            var reply = await context.SpotRequest(owner,
                    new RemoteSpotAwaitReq(ExecutionTurnScenarioContext.NewId("TD-F1"), target, 100))
                .Async<AutomaticTurnDispatchRes>();
            ZlinkStreamAssert.Ensure(
                reply.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
                "TD-F1 continuation did not return to the caller node.");
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
    }
}
