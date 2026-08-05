// Verifies TD-F4 Request Timeout Recovery behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdF4RequestTimeoutRecoveryScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spotRid = $"await-timeout-{Guid.NewGuid():N}";
        await context.EnsureSpotAsync(spotRid, "play-a");
        var requestId = $"TD-F4-{Guid.NewGuid():N}";
        await context.SendSpotAsync(new AwaitTimeoutMsg(requestId, 700, 100), spotRid);
        await context.EvidenceAsync(requestId, "timeout-await-completed");
        await context.SendSpotAsync(new ProbeMsg(requestId, "timeout-probe"), spotRid);
        var evidence = await context.EvidenceAsync(requestId, "probe-completed");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                 && line.Contains("timeout-await-completed", StringComparison.Ordinal)),
            "TD-F4 timeout marker missing.");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                 && line.Contains("probe-completed", StringComparison.Ordinal)
                                 && line.Contains("timeout-probe", StringComparison.Ordinal)),
            "TD-F4 post-timeout probe marker missing.");
    }
}
