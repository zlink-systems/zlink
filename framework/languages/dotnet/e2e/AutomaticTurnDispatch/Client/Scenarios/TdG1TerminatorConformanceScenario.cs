// Verifies TD-G1 Terminator Conformance behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdG1TerminatorConformanceScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        await TdA1TerminatorSurfaceScenario.RunAsync(context);
        await VerifyAsync(context, "async", false);
        await VerifyAsync(context, "yield", true);
    }

    private static async Task VerifyAsync(ExecutionTurnScenarioContext context, string terminator, bool probeDuringWait)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-G1");
        await context.SendSpotAsync(new AwaitMsg(requestId, 300, "TD-G1", terminator), spot);
        await context.EvidenceAsync(requestId, probeDuringWait ? "yield-released" : "await-held");
        await context.SendSpotAsync(new ProbeMsg(requestId, "interleave-probe"), spot);
        await context.EvidenceAsync(requestId, probeDuringWait ? "yield-completed" : "async-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId, probeDuringWait
            ? ["yield-released", "probe-started", "probe-completed", "yield-resumed"]
            : ["await-held", "async-resumed", "async-completed", "probe-started", "probe-completed"]);
    }
}
