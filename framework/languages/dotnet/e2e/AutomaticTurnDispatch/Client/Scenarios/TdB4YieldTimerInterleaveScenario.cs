// Verifies TD-B4 Yield Timer Interleave behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdB4YieldTimerInterleaveScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-B4");
        await context.SendSpotAsync(new TimerStartMsg(requestId, requestId, "fast", 40, 0), spot);
        await context.EvidenceAsync(requestId, "timer-started");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "TD-B4", "yield"), spot);
        await context.EvidenceAsync(requestId, "yield-released");
        await context.EvidenceAsync(requestId, "yield-completed");
        var evidence = await context.EvidenceAsync(requestId, "timer-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["yield-released", "timer-fast-started", "timer-fast-completed", "yield-resumed"]);
        await context.SendSpotAsync(new TimerStopMsg(requestId), spot);
    }
}
