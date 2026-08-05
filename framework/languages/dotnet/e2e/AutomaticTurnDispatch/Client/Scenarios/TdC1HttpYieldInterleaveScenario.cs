// Verifies TD-C1 Http Yield Interleave behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdC1HttpYieldInterleaveScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-C1");
        await context.SendSpotAsync(new HttpAwaitMsg(requestId, 300, "yield"), spot);
        await context.EvidenceAsync(requestId, "http-yield-released");
        await context.SendSpotAsync(new ProbeMsg(requestId, "http-probe"), spot);
        await context.EvidenceAsync(requestId, "http-yield-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["http-yield-released", "probe-started", "probe-completed", "http-yield-resumed"]);
    }
}
