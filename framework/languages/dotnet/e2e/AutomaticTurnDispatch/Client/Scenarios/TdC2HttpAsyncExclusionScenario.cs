// Verifies TD-C2 Http Async Exclusion behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdC2HttpAsyncExclusionScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-C2");
        await context.SendSpotAsync(new HttpAwaitMsg(requestId, 300, "async"), spot);
        await context.EvidenceAsync(requestId, "http-async-held");
        await context.SendSpotAsync(new ProbeMsg(requestId, "http-probe"), spot);
        await context.EvidenceAsync(requestId, "http-async-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["http-async-held", "http-async-completed", "probe-started", "probe-completed"]);
    }
}
