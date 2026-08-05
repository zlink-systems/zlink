// Verifies TD-B2 Yield Queued Probe Order behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdB2YieldQueuedProbeOrderScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-B2");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "queue-order", "yield"), spot);
        await context.EvidenceAsync(requestId, "yield-released");
        for (var index = 1; index <= 3; index++)
            await context.SendSpotAsync(new ProbeMsg(requestId, $"probe-{index}"), spot);
        var evidence = await context.EvidenceAsync(requestId, "yield-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
        [
            "yield-released", "marker=probe-1", "marker=probe-2", "marker=probe-3", "yield-resumed",
            "yield-completed"
        ]);
    }
}
