// Verifies TD-D3 Timer No Reentry behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdD3TimerNoReentryScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-D3");
        await context.SendSpotAsync(new TimerStartMsg(requestId, requestId, "yield-on-first", 40, 250), spot);
        var evidence = await context.EvidenceAsync(requestId, "timer-next-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["timer-yield-released", "timer-yield-resumed", "timer-yield-completed", "timer-next-started"]);
        await context.SendSpotAsync(new TimerStopMsg(requestId), spot);
    }
}
