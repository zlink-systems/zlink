// Verifies TD-A5 Async Timer Exclusion behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdA5AsyncTimerExclusionScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-A5");
        await context.SendSpotAsync(new TimerStartMsg(requestId, requestId, "fast", 40, 0), spot);
        await context.EvidenceAsync(requestId, "timer-started");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "TD-A5", "async"), spot);
        await context.EvidenceAsync(requestId, "await-held");
        await context.EvidenceAsync(requestId, "async-completed");
        var evidence = await context.EvidenceAsync(requestId, "timer-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["await-held", "async-completed", "timer-fast-started", "timer-fast-completed"]);
        await context.SendSpotAsync(new TimerStopMsg(requestId), spot);
    }
}
