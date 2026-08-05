// Verifies TD-A4 Delayed Async Completion behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdA4DelayedAsyncCompletionScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-A4");
        var reply = await context.SpotRequest(spot,
            new AwaitReq(requestId, 1000, "completion-axis", "async"),
            TimeSpan.FromSeconds(5)).Async<AutomaticTurnDispatchRes>();
        ZlinkStreamAssert.Ensure(reply.Marker == "async-completed", "TD-A4 async completion did not resume.");
    }
}
