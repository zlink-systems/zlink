// Verifies TD-F5 Cancellation Shutdown Recovery behavior.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdF5CancellationShutdownRecoveryScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spotRid = $"await-cancel-{Guid.NewGuid():N}";
        await context.EnsureSpotAsync(spotRid, "play-a");
        var requestId = $"TD-F5-{Guid.NewGuid():N}";
        await context.SendSpotAsync(new AwaitCancelMsg(requestId, 800, 100), spotRid);
        await context.EvidenceAsync(requestId, "cancel-await-completed");
        await context.SendSpotAsync(new ProbeMsg(requestId, "cancel-probe"), spotRid);
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(evidence, requestId,
        [
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed",
            "probe-started",
            "probe-completed"
        ]);
    }
}
