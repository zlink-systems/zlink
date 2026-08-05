// Verifies TD-C3 Io Worker Capacity behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdC3IoWorkerCapacityScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-C3");
        for (var index = 0; index < 32; index++)
            await context.SendSpotAsync(new IoWorkerAwaitMsg(requestId, $"io-{index:D2}", 150), spot);
        var evidence = await context.EvidenceAsync(requestId, "io-worker-completed", minimumCount: 32);
        ZlinkStreamAssert.Ensure(
            evidence.Count(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                   && line.Contains("io-worker-completed", StringComparison.Ordinal)) == 32,
            "TD-C3 did not complete all I/O workers.");
        ZlinkStreamAssert.Ensure(evidence.All(line => !line.Contains("WorkerQueueFull", StringComparison.Ordinal)),
            "TD-C3 exhausted the CPU worker queue.");
    }
}
