// Verifies TD-B1 Yield Probe And Timer Interleave behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdB1YieldProbeInterleaveScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        await VerifyProbeInterleaveAsync(context);
        await VerifyTimerInterleaveAsync(context);
    }

    private static async Task VerifyProbeInterleaveAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-B1");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "TD-B1", "yield"), spot);
        await context.EvidenceAsync(requestId, "yield-released");
        await context.SendSpotAsync(new ProbeMsg(requestId, "interleave-probe"), spot);
        await context.EvidenceAsync(requestId, "yield-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["yield-released", "probe-started", "probe-completed", "yield-resumed"]);
    }

    private static async Task VerifyTimerInterleaveAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-B1-timer");
        await context.SendSpotAsync(new TimerStartMsg(requestId, requestId, "fast", 40, 0), spot);
        await context.EvidenceAsync(requestId, "timer-started");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "TD-B1", "yield"), spot);
        await context.EvidenceAsync(requestId, "yield-released");
        await context.EvidenceAsync(requestId, "yield-completed");
        var evidence = await context.EvidenceAsync(requestId, "timer-fast-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["yield-released", "timer-fast-started", "timer-fast-completed", "yield-resumed"]);
        await context.SendSpotAsync(new TimerStopMsg(requestId), spot);
    }
}
