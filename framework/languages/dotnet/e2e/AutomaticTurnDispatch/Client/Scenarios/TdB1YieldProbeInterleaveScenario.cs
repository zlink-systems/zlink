// Verifies TD-B1 Yield Probe Interleave behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdB1YieldProbeInterleaveScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
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
}
