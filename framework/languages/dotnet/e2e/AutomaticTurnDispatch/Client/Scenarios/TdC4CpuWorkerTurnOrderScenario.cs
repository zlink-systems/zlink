// Verifies TD-C4 Cpu Worker Turn Order behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdC4CpuWorkerTurnOrderScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        await VerifyAsync(context, "TD-C4-async", "async", false);
        await VerifyAsync(context, "TD-C4-yield", "yield", true);
    }

    private static async Task VerifyAsync(
        ExecutionTurnScenarioContext context, string scenarioId, string terminator, bool probeDuringWait)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId(scenarioId);
        await context.SendSpotAsync(new CpuWorkerAwaitMsg(requestId, 250, terminator), spot);
        await context.EvidenceAsync(requestId, $"cpu-worker-{terminator}-{(probeDuringWait ? "released" : "held")}");
        await context.SendSpotAsync(new ProbeMsg(requestId, "cpu-probe"), spot);
        await context.EvidenceAsync(requestId, $"cpu-worker-{terminator}-completed");
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId, probeDuringWait
            ? ["cpu-worker-yield-released", "probe-started", "probe-completed", "cpu-worker-yield-completed"]
            : ["cpu-worker-async-held", "cpu-worker-async-completed", "probe-started", "probe-completed"]);
        var completion = evidence.First(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                                && line.Contains("cpu-worker", StringComparison.Ordinal)
                                                && line.Contains("completed", StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(!completion.Contains("caller-thread=0|worker-thread=0", StringComparison.Ordinal),
            $"{scenarioId} did not record CPU worker thread evidence.");
    }
}
