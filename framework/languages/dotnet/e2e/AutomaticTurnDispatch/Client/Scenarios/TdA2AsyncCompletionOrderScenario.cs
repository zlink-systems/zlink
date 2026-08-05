// Verifies TD-A2 Async Completion Order behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdA2AsyncCompletionOrderScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        const string scenarioId = "TD-A2";
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId(scenarioId);
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, scenarioId, "async"), spot);
        await context.EvidenceAsync(requestId, "await-held");
        await context.SendSpotAsync(new ProbeMsg(requestId, "interleave-probe"), spot);
        await context.EvidenceAsync(requestId, "async-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
            ["await-held", "async-resumed", "async-completed", "probe-started", "probe-completed"]);
    }
}
